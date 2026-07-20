#pragma once

// ===========================================================================
//  APP VERSION -- bump this every time you publish a new GitHub release.
//
//  The auto-updater compares this against the newest release's git tag
//  (e.g. tag "v1.2.0") fetched from the GitHub API. If the release tag is
//  higher than the number below, the running app offers to update itself.
//
//  Keep this in sync with the tag you create on GitHub:
//    build with APP_VERSION_STR = "1.1.0"  ->  publish release tagged "v1.1.0"
// ===========================================================================
#define APP_VERSION_MAJOR 1
#define APP_VERSION_MINOR 6
#define APP_VERSION_PATCH 4

#define APP_VERSION_STR "1.6.4"
