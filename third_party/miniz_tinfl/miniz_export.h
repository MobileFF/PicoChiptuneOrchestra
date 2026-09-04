/* Hand-written replacement for the file CMake's GenerateExportHeader module
 * would normally produce for a shared-library build of miniz. We only ever
 * build miniz_tinfl.c into a static executable, so every export macro is a
 * no-op. */
#ifndef MINIZ_EXPORT_H
#define MINIZ_EXPORT_H

#define MINIZ_EXPORT
#define MINIZ_NO_EXPORT
#define MINIZ_DEPRECATED

#endif
