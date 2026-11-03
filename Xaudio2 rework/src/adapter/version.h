/* 2.1 T2: the adapter version, the one source for the create log line, VinceAudio.rc and the build's FileVersion check.
 * Numeric defines only (build.ps1 parses them); VA_VERSION_STR is built from them, never written as a literal. */
#ifndef VA_VERSION_H
#define VA_VERSION_H
#define VA_VERSION_MAJOR 1
#define VA_VERSION_MINOR 3 /* was 2 (5.1 T2 MasterVolume default 0.5); was 1 (4.1 T4 upgrade with D); was 0 (3.1 T1 MasterVolume ini) */
#define VA_VERSION_PATCH 0
#define VA_VERSION_STR2(x) #x
#define VA_VERSION_STR1(x) VA_VERSION_STR2(x)
#define VA_VERSION_STR VA_VERSION_STR1(VA_VERSION_MAJOR) "." VA_VERSION_STR1(VA_VERSION_MINOR) "." VA_VERSION_STR1(VA_VERSION_PATCH)
#endif
