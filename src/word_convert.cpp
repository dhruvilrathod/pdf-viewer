#include "word_convert.h"

#include <windows.h>
#include <objbase.h>
#include <oleauto.h>

namespace {

// Late-bound IDispatch call helpers -- avoid needing Word's type library /
// #import at build time (which would require Office to be installed on the
// BUILD machine, not just the end user's machine that eventually runs this).

HRESULT InvokeMethod(IDispatch* disp, const wchar_t* name, WORD flags,
	VARIANT* args, UINT argCount, VARIANT* result)
{
	if (!disp) return E_POINTER;
	OLECHAR* nameNonConst = const_cast<OLECHAR*>(name); // GetIDsOfNames wants LPOLESTR*, not const
	DISPID dispid;
	HRESULT hr = disp->GetIDsOfNames(IID_NULL, &nameNonConst, 1, LOCALE_USER_DEFAULT, &dispid);
	if (FAILED(hr)) return hr;
	DISPPARAMS params = {};
	params.cArgs = argCount;
	params.rgvarg = args; // caller supplies these in REVERSE order, per IDispatch::Invoke's contract
	DISPID namedPut = DISPID_PROPERTYPUT;
	if (flags & (DISPATCH_PROPERTYPUT | DISPATCH_PROPERTYPUTREF)) {
		params.cNamedArgs = 1;
		params.rgdispidNamedArgs = &namedPut;
	}
	return disp->Invoke(dispid, IID_NULL, LOCALE_USER_DEFAULT, flags, &params, result, nullptr, nullptr);
}

HRESULT CallMethod(IDispatch* disp, const wchar_t* name, VARIANT* args = nullptr, UINT argCount = 0,
	VARIANT* result = nullptr)
{
	return InvokeMethod(disp, name, DISPATCH_METHOD, args, argCount, result);
}

HRESULT PutProperty(IDispatch* disp, const wchar_t* name, VARIANT value)
{
	return InvokeMethod(disp, name, DISPATCH_PROPERTYPUT, &value, 1, nullptr);
}

HRESULT GetProperty(IDispatch* disp, const wchar_t* name, VARIANT* result)
{
	return InvokeMethod(disp, name, DISPATCH_PROPERTYGET, nullptr, 0, result);
}

VARIANT BoolVariant(bool b)
{
	VARIANT v; VariantInit(&v);
	v.vt = VT_BOOL;
	v.boolVal = b ? VARIANT_TRUE : VARIANT_FALSE;
	return v;
}

} // namespace

bool ConvertDocxToPdf(const wchar_t* docxPath, const wchar_t* pdfPath, std::string& err)
{
	HRESULT hrInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
	if (!(SUCCEEDED(hrInit) || hrInit == RPC_E_CHANGED_MODE)) {
		err = "could not initialize COM";
		return false;
	}
	bool comOwned = SUCCEEDED(hrInit);

	IDispatch* wordApp = nullptr;
	{
		CLSID clsid;
		if (FAILED(CLSIDFromProgID(L"Word.Application", &clsid))) {
			err = "Microsoft Word is not installed (required to convert .docx files)";
			if (comOwned) CoUninitialize();
			return false;
		}
		// LOCAL_SERVER: Word.Application is an out-of-process EXE automation
		// server, not an in-proc DLL.
		HRESULT hr = CoCreateInstance(clsid, nullptr, CLSCTX_LOCAL_SERVER, IID_IDispatch,
			reinterpret_cast<void**>(&wordApp));
		if (FAILED(hr) || !wordApp) {
			err = "could not start Microsoft Word";
			if (comOwned) CoUninitialize();
			return false;
		}
	}

	// Headless: this is a one-shot conversion, not an interactive session --
	// stay hidden and suppress "convert/repair?"-style prompts that would
	// otherwise block forever with no user there to answer them.
	{
		VARIANT v = BoolVariant(false);
		PutProperty(wordApp, L"Visible", v);
	}
	{
		VARIANT v = BoolVariant(false);
		PutProperty(wordApp, L"DisplayAlerts", v);
	}

	bool ok = false;
	IDispatch* documents = nullptr;
	{
		VARIANT result; VariantInit(&result);
		if (SUCCEEDED(GetProperty(wordApp, L"Documents", &result)) && result.vt == VT_DISPATCH)
			documents = result.pdispVal; // ownership transferred to us
	}
	if (!documents) {
		err = "could not access Word's document list";
	} else {
		IDispatch* doc = nullptr;
		{
			VARIANT arg; VariantInit(&arg);
			arg.vt = VT_BSTR;
			arg.bstrVal = SysAllocString(docxPath);
			VARIANT result; VariantInit(&result);
			HRESULT hr = InvokeMethod(documents, L"Open", DISPATCH_METHOD, &arg, 1, &result);
			SysFreeString(arg.bstrVal);
			if (SUCCEEDED(hr) && result.vt == VT_DISPATCH) doc = result.pdispVal;
		}
		documents->Release();

		if (!doc) {
			err = "Word could not open the document (it may be corrupt or password-protected)";
		} else {
			// SaveAs2(FileName, FileFormat, ...) -- rgvarg is reverse-order:
			// [0]=FileFormat (17 = wdFormatPDF), [1]=FileName.
			VARIANT args[2];
			VariantInit(&args[0]); args[0].vt = VT_I4; args[0].lVal = 17; // wdFormatPDF
			VariantInit(&args[1]); args[1].vt = VT_BSTR; args[1].bstrVal = SysAllocString(pdfPath);
			HRESULT hr = InvokeMethod(doc, L"SaveAs2", DISPATCH_METHOD, args, 2, nullptr);
			SysFreeString(args[1].bstrVal);
			ok = SUCCEEDED(hr);
			if (!ok) err = "Word failed to save the PDF";

			VARIANT saveChanges = BoolVariant(false); // never write back to the source .docx
			CallMethod(doc, L"Close", &saveChanges, 1);
			doc->Release();
		}
	}

	CallMethod(wordApp, L"Quit");
	wordApp->Release();
	if (comOwned) CoUninitialize();
	return ok;
}
