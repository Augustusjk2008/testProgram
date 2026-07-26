"""Rich-text log writer with the fixed hardware-test color palette."""

from typing import Optional

from PyQt5.QtGui import QColor, QTextCharFormat, QTextCursor
from PyQt5.QtWidgets import QTextEdit


LOG_COLORS = {
    "TX": "#1565C0",
    "RX": "#00838F",
    "COMPLETE": "#2E7D32",
    "WARNING": "#EF6C00",
    "ERROR": "#C62828",
    "INFO": "#424242",
}


class ColoredLog:
    """Append escaped text to a ``QTextEdit`` without unbounded growth."""

    def __init__(
        self,
        widget: QTextEdit,
        maximum_blocks: int = 5000,
    ) -> None:
        if maximum_blocks <= 0:
            raise ValueError("日志块上限必须大于 0")
        self.widget = widget
        self.widget.setReadOnly(True)
        self.widget.document().setMaximumBlockCount(maximum_blocks)

    def append(self, category: str, message: object) -> None:
        level = str(category).upper()
        color = LOG_COLORS.get(level, LOG_COLORS["INFO"])
        cursor = self.widget.textCursor()
        cursor.movePosition(QTextCursor.End)
        if self.widget.toPlainText():
            cursor.insertBlock()
        char_format = QTextCharFormat()
        char_format.setForeground(QColor(color))
        cursor.insertText(str(message), char_format)
        self.widget.setTextCursor(cursor)
        self.widget.ensureCursorVisible()


__all__ = ["ColoredLog", "LOG_COLORS"]
