#pragma once

#include <QtCore/qglobal.h>

#if defined(HWTEST_LOG_STATIC)
#  define HWTEST_LOG_EXPORT
#elif defined(HWTEST_LOG_LIBRARY)
#  define HWTEST_LOG_EXPORT Q_DECL_EXPORT
#else
#  define HWTEST_LOG_EXPORT Q_DECL_IMPORT
#endif
