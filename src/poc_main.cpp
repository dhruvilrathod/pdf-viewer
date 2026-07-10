// Minimal proof-of-concept: open a PDF with statically-linked MuPDF and
// render page 1 to a PNG. Confirms the CMake build in cmake/MuPDF.cmake
// actually produces a working, self-contained mupdf.lib before any UI
// code is written.
#include <cstdio>

extern "C" {
#include <mupdf/fitz.h>
}

int main(int argc, char** argv)
{
	if (argc < 2) {
		std::fprintf(stderr, "usage: %s input.pdf [output.png]\n", argv[0]);
		return 1;
	}
	const char* input = argv[1];
	const char* output = argc > 2 ? argv[2] : "poc_output.png";

	fz_context* ctx = fz_new_context(nullptr, nullptr, FZ_STORE_UNLIMITED);
	if (!ctx) {
		std::fprintf(stderr, "failed to create mupdf context\n");
		return 1;
	}
	fz_register_document_handlers(ctx);

	fz_document* doc = nullptr;
	fz_page* page = nullptr;
	fz_pixmap* pix = nullptr;

	fz_try(ctx) {
		doc = fz_open_document(ctx, input);
		int page_count = fz_count_pages(ctx, doc);
		std::printf("opened '%s': %d page(s)\n", input, page_count);

		page = fz_load_page(ctx, doc, 0);
		fz_rect bounds = fz_bound_page(ctx, page);
		std::printf("page 1 size: %g x %g pt\n", bounds.x1 - bounds.x0, bounds.y1 - bounds.y0);

		fz_matrix ctm = fz_scale(150.0f / 72.0f, 150.0f / 72.0f); // 150 DPI
		pix = fz_new_pixmap_from_page(ctx, page, ctm, fz_device_rgb(ctx), 0);
		fz_save_pixmap_as_png(ctx, pix, output);
		std::printf("wrote %s (%d x %d)\n", output, fz_pixmap_width(ctx, pix), fz_pixmap_height(ctx, pix));
	}
	fz_always(ctx) {
		fz_drop_pixmap(ctx, pix);
		fz_drop_page(ctx, page);
		fz_drop_document(ctx, doc);
	}
	fz_catch(ctx) {
		std::fprintf(stderr, "error: %s\n", fz_caught_message(ctx));
		fz_drop_context(ctx);
		return 1;
	}

	fz_drop_context(ctx);
	return 0;
}
