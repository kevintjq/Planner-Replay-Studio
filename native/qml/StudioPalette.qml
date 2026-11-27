import QtQuick

Palette {
    window: appearance.colors.window
    windowText: appearance.colors.text
    base: appearance.colors.input
    alternateBase: appearance.colors.control
    text: appearance.colors.text
    button: appearance.colors.control
    buttonText: appearance.colors.text
    highlight: appearance.colors.accentFill
    highlightedText: appearance.colors.accentText
    placeholderText: appearance.colors.secondaryText
    toolTipBase: appearance.colors.tooltip
    toolTipText: appearance.colors.text
    light: appearance.colors.hover
    midlight: appearance.colors.control
    mid: appearance.colors.border
    dark: appearance.colors.separator
    link: appearance.colors.accent
    accent: appearance.colors.accent
    disabled {
        windowText: appearance.colors.disabledText
        text: appearance.colors.disabledText
        buttonText: appearance.colors.disabledText
        placeholderText: appearance.colors.disabledText
    }
}
