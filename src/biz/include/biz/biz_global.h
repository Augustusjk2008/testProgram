#pragma once

#include <QtCore/qglobal.h>

#if defined(HWTEST_BIZ_STATIC)
#  define HWTEST_BIZ_EXPORT
#elif defined(HWTEST_BIZ_LIBRARY)
#  define HWTEST_BIZ_EXPORT Q_DECL_EXPORT
#else
#  define HWTEST_BIZ_EXPORT Q_DECL_IMPORT
#endif
