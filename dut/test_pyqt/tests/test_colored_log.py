from PyQt5.QtWidgets import QTextEdit

from test_pyqt.colored_log import LOG_COLORS, ColoredLog


def test_colored_log_uses_fixed_palette_and_block_limit(qtbot) -> None:
    assert LOG_COLORS == {
        "TX": "#1565C0",
        "RX": "#00838F",
        "COMPLETE": "#2E7D32",
        "WARNING": "#EF6C00",
        "ERROR": "#C62828",
        "INFO": "#424242",
    }
    widget = QTextEdit()
    qtbot.addWidget(widget)
    log = ColoredLog(widget, maximum_blocks=3)

    log.append("TX", "tx line")
    log.append("RX", "rx line")
    log.append("COMPLETE", "complete line")
    log.append("ERROR", "error line")

    assert widget.document().maximumBlockCount() == 3
    plain = widget.toPlainText()
    assert "tx line" not in plain
    assert "error line" in plain
    html = widget.toHtml().lower()
    assert LOG_COLORS["RX"].lower() in html
    assert LOG_COLORS["COMPLETE"].lower() in html
    assert LOG_COLORS["ERROR"].lower() in html


def test_colored_log_inserts_untrusted_text_as_text(qtbot) -> None:
    widget = QTextEdit()
    qtbot.addWidget(widget)
    log = ColoredLog(widget)

    log.append("INFO", "<b>literal</b>")

    assert "<b>literal</b>" in widget.toPlainText()
