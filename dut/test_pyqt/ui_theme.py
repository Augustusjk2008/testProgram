"""Shared, Win7-compatible visual constants for the PC hardware-test tool."""

from typing import Dict


COLORS: Dict[str, str] = {
    "window": "#F3F5F6",
    "surface": "#FFFFFF",
    "surface_subtle": "#F7F8F8",
    "surface_pressed": "#E7EAEC",
    "border": "#C8CDD0",
    "border_strong": "#8A9199",
    "text": "#263238",
    "text_muted": "#5F6B70",
    "primary": "#006D77",
    "primary_hover": "#005A63",
    "primary_pressed": "#004B53",
    "primary_subtle": "#D7EEF0",
    "disabled": "#A3AAAE",
    "tx": "#1565C0",
    "rx": "#00838F",
    "complete": "#2E7D32",
    "warning": "#EF6C00",
    "error": "#C62828",
    "info": "#424242",
}

CORNER_RADIUS = 4
CONTROL_HEIGHT = 30
COMPACT_CONTROL_HEIGHT = 26
PAGE_MARGIN = 12
SECTION_SPACING = 10


APP_STYLE_SHEET = """
QWidget {
    color: #263238;
    background-color: #F3F5F6;
    selection-background-color: #006D77;
    selection-color: #FFFFFF;
}

QMainWindow, QDialog {
    background-color: #F3F5F6;
}

QLabel {
    background-color: transparent;
}

QLabel[muted="true"] {
    color: #5F6B70;
}

QGroupBox {
    margin-top: 9px;
    padding: 10px 8px 8px 8px;
    border: 1px solid #C8CDD0;
    border-radius: 4px;
    background-color: #FFFFFF;
    font-weight: bold;
}

QGroupBox::title {
    subcontrol-origin: margin;
    subcontrol-position: top left;
    left: 8px;
    padding: 0 4px;
    color: #424242;
    background-color: #F3F5F6;
}

QPushButton, QToolButton {
    min-height: 30px;
    padding: 0 10px;
    border: 1px solid #AEB4B8;
    border-radius: 4px;
    background-color: #FFFFFF;
    color: #263238;
}

QPushButton:hover, QToolButton:hover {
    border-color: #8A9199;
    background-color: #F7F8F8;
}

QPushButton:pressed, QToolButton:pressed {
    background-color: #E7EAEC;
}

QPushButton:disabled, QToolButton:disabled {
    color: #A3AAAE;
    border-color: #D7DADC;
    background-color: #EFF1F2;
}

QPushButton[primary="true"] {
    border-color: #006D77;
    background-color: #006D77;
    color: #FFFFFF;
    font-weight: bold;
}

QPushButton[primary="true"]:hover {
    border-color: #005A63;
    background-color: #005A63;
}

QPushButton[primary="true"]:pressed {
    border-color: #004B53;
    background-color: #004B53;
}

QPushButton[primary="true"]:disabled {
    border-color: #A7C4C7;
    background-color: #A7C4C7;
    color: #F4F6F6;
}

QPushButton[segmented="true"] {
    min-height: 28px;
    border-radius: 0;
    border-right-width: 0;
    padding: 0 12px;
}

QPushButton[segmented="true"][segmentPosition="first"] {
    border-top-left-radius: 4px;
    border-bottom-left-radius: 4px;
}

QPushButton[segmented="true"][segmentPosition="last"] {
    border-right-width: 1px;
    border-top-right-radius: 4px;
    border-bottom-right-radius: 4px;
}

QPushButton[segmented="true"]:checked {
    border-color: #006D77;
    background-color: #006D77;
    color: #FFFFFF;
    font-weight: bold;
}

QLineEdit, QComboBox, QSpinBox, QDoubleSpinBox {
    min-height: 28px;
    padding: 0 7px;
    border: 1px solid #AEB4B8;
    border-radius: 4px;
    background-color: #FFFFFF;
    color: #263238;
}

QLineEdit:focus, QComboBox:focus, QSpinBox:focus, QDoubleSpinBox:focus {
    border: 2px solid #006D77;
    padding-left: 6px;
    padding-right: 6px;
}

QLineEdit:disabled, QComboBox:disabled, QSpinBox:disabled,
QDoubleSpinBox:disabled {
    color: #8B9296;
    border-color: #D7DADC;
    background-color: #EFF1F2;
}

QComboBox::drop-down, QSpinBox::up-button, QSpinBox::down-button,
QDoubleSpinBox::up-button, QDoubleSpinBox::down-button {
    border: 0;
    background-color: transparent;
}

QCheckBox {
    spacing: 6px;
    background-color: transparent;
}

QCheckBox::indicator {
    width: 16px;
    height: 16px;
}

QTabWidget::pane {
    border: 1px solid #C8CDD0;
    background-color: #FFFFFF;
}

QTabBar::tab {
    min-width: 76px;
    min-height: 30px;
    padding: 5px 10px;
    border: 1px solid transparent;
    background-color: #E9ECEE;
    color: #424B4F;
}

QTabBar::tab:hover {
    background-color: #DDE2E4;
}

QTabBar::tab:selected {
    border-color: #C8CDD0;
    background-color: #FFFFFF;
    color: #006D77;
    font-weight: bold;
}

QTableView, QTreeView, QListView, QTextEdit, QPlainTextEdit {
    border: 1px solid #C8CDD0;
    border-radius: 4px;
    background-color: #FFFFFF;
    alternate-background-color: #F7F8F8;
    color: #263238;
}

QTableView::item, QTreeView::item, QListView::item {
    min-height: 25px;
    padding: 2px 5px;
}

QTableView::item:selected, QTreeView::item:selected, QListView::item:selected {
    background-color: #D7EEF0;
    color: #263238;
}

QHeaderView::section {
    min-height: 28px;
    padding: 3px 6px;
    border: 0;
    border-right: 1px solid #D6DADC;
    border-bottom: 1px solid #C8CDD0;
    background-color: #E9ECEE;
    color: #424242;
    font-weight: bold;
}

QScrollArea, QScrollArea > QWidget > QWidget {
    border: 0;
    background-color: transparent;
}

QScrollBar:vertical {
    width: 13px;
    margin: 0;
    border: 0;
    background-color: #ECEFF0;
}

QScrollBar::handle:vertical {
    min-height: 28px;
    margin: 2px;
    border-radius: 4px;
    background-color: #AEB5B9;
}

QScrollBar:horizontal {
    height: 13px;
    margin: 0;
    border: 0;
    background-color: #ECEFF0;
}

QScrollBar::handle:horizontal {
    min-width: 28px;
    margin: 2px;
    border-radius: 4px;
    background-color: #AEB5B9;
}

QScrollBar::add-line, QScrollBar::sub-line,
QScrollBar::add-page, QScrollBar::sub-page {
    width: 0;
    height: 0;
    background-color: transparent;
}

QSplitter::handle {
    background-color: #DDE1E3;
}

QFrame[metricPanel="true"], QFrame[bitCell="true"] {
    border: 1px solid #D3D7D9;
    border-radius: 4px;
    background-color: #F7F8F8;
}

QLabel[metricName="true"], QLabel[bitName="true"] {
    color: #5F6B70;
}

QLabel[metricValue="true"] {
    color: #263238;
    font-size: 15px;
    font-weight: bold;
}

QLabel[metricValue="true"][available="false"] {
    color: #8B9296;
    font-size: 13px;
    font-weight: normal;
}

QFrame[bitCell="true"][state="active"] {
    border-color: #006D77;
    background-color: #D7EEF0;
}

QLabel[bitState="true"][state="active"] {
    color: #006D77;
    font-weight: bold;
}

QLabel[bitState="true"][state="inactive"] {
    color: #424B4F;
    font-weight: bold;
}

QLabel[bitState="true"][state="unavailable"] {
    color: #8B9296;
}

QLabel[pageStatus="true"] {
    min-height: 25px;
    padding: 0 8px;
    border: 1px solid #C8CDD0;
    border-radius: 4px;
    background-color: #F7F8F8;
    font-weight: bold;
}

QLabel[pageStatus="true"][statusLevel="running"] {
    border-color: #1565C0;
    color: #1565C0;
}

QLabel[pageStatus="true"][statusLevel="complete"] {
    border-color: #2E7D32;
    color: #2E7D32;
}

QLabel[pageStatus="true"][statusLevel="warning"] {
    border-color: #EF6C00;
    color: #EF6C00;
}

QLabel[pageStatus="true"][statusLevel="error"] {
    border-color: #C62828;
    color: #C62828;
}

QToolTip {
    padding: 4px 6px;
    border: 1px solid #8A9199;
    background-color: #FFFFFF;
    color: #263238;
}
"""

# Keep common import spellings stable while the UI shell is being migrated.
APPLICATION_STYLE_SHEET = APP_STYLE_SHEET
STYLE_SHEET = APP_STYLE_SHEET
QSS = APP_STYLE_SHEET


def apply_theme(application) -> None:
    """Apply the static application QSS without relying on platform effects."""

    application.setStyleSheet(APP_STYLE_SHEET)


__all__ = [
    "APP_STYLE_SHEET",
    "APPLICATION_STYLE_SHEET",
    "COLORS",
    "COMPACT_CONTROL_HEIGHT",
    "CONTROL_HEIGHT",
    "CORNER_RADIUS",
    "PAGE_MARGIN",
    "QSS",
    "SECTION_SPACING",
    "STYLE_SHEET",
    "apply_theme",
]
