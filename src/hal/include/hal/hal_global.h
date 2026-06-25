#pragma once

#include <QtCore/qglobal.h>

#if defined(HWTEST_HAL_STATIC)
#  define HWTEST_HAL_EXPORT
#elif defined(HWTEST_HAL_LIBRARY)
#  define HWTEST_HAL_EXPORT Q_DECL_EXPORT
#else
#  define HWTEST_HAL_EXPORT Q_DECL_IMPORT
#endif
