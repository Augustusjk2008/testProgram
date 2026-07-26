"""Application entry point with the deliberately pinned Win7 interpreter."""

import os
import sys
from typing import Optional, Sequence


EXPECTED_PYTHON = r"C:\Users\JiangKai\.conda\envs\pyqt5_env\python.exe"
EXPECTED_VERSION = (3, 8)


def validate_runtime() -> Optional[str]:
    if sys.version_info[:2] != EXPECTED_VERSION:
        return "Python 版本不正确：必须使用 Python 3.8，当前为 {}.{}".format(
            sys.version_info[0], sys.version_info[1]
        )
    if getattr(sys, "frozen", False):
        return None

    actual = os.path.normcase(os.path.abspath(sys.executable))
    expected = os.path.normcase(os.path.abspath(EXPECTED_PYTHON))
    if actual != expected:
        return "解释器路径不正确：必须使用 {}，当前为 {}".format(
            EXPECTED_PYTHON, sys.executable
        )
    return None


def configure_application(app) -> None:
    """Apply process-wide settings after QApplication construction."""

    from PyQt5.QtGui import QFont, QFontDatabase

    from .ui_theme import apply_theme

    if "Microsoft YaHei" not in QFontDatabase().families():
        fonts_directory = os.path.join(
            os.environ.get("WINDIR", r"C:\Windows"), "Fonts"
        )
        for filename in ("msyh.ttc", "msyh.ttf"):
            font_path = os.path.join(fonts_directory, filename)
            if os.path.isfile(font_path):
                if QFontDatabase.addApplicationFont(font_path) >= 0:
                    break

    app.setFont(QFont("Microsoft YaHei"))
    app.setApplicationName("MB_DDF COM3 硬件测试")
    app.setOrganizationName("MB_DDF")
    apply_theme(app)


def main(argv: Optional[Sequence[str]] = None) -> int:
    error = validate_runtime()
    if error is not None:
        print("错误：{}".format(error), file=sys.stderr)
        return 2

    from PyQt5.QtWidgets import QApplication

    from .main_window import MainWindow

    app = QApplication(list(argv) if argv is not None else sys.argv)
    configure_application(app)
    window = MainWindow()
    window.show()
    return app.exec_()
