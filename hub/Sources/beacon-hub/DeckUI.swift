import SwiftUI

// Shared "Hub Deck" visual vocabulary used by every SwiftUI surface (the menu panel, Setup, Forget) so
// they stay consistent. Adaptive/system-native: subtle fills over the host window/popover material, the
// blue accent for primary actions, no forced dark theme.

// Rounded module card: a faint fill that reads as a grouped surface in both light and dark.
struct Module<Content: View>: View {
    var padding: CGFloat = 11
    @ViewBuilder var content: Content
    var body: some View {
        content
            .padding(padding)
            .background(.primary.opacity(0.06), in: RoundedRectangle(cornerRadius: 13, style: .continuous))
    }
}

// A rounded text button in two weights: .secondary (subtle, bordered) and .primary (blue, the default
// action). `large` sizes footer buttons up from the inline row actions.
enum DeckButtonKind { case secondary, primary }

struct DeckButton: View {
    let title: String
    var kind: DeckButtonKind = .secondary
    var large: Bool = false
    var enabled: Bool = true
    let action: () -> Void

    var body: some View {
        Button(title, action: action)
            .buttonStyle(DeckButtonStyle(kind: kind, large: large))
            .disabled(!enabled)
            .opacity(enabled ? 1 : 0.5)
    }
}

private struct DeckButtonStyle: ButtonStyle {
    let kind: DeckButtonKind
    let large: Bool

    func makeBody(configuration: Configuration) -> some View {
        let shape = RoundedRectangle(cornerRadius: 7, style: .continuous)
        return configuration.label
            .font(.system(size: large ? 13 : 12, weight: .medium))
            .padding(.horizontal, large ? 14 : 11)
            .padding(.vertical, large ? 6 : 5)
            .background(background(pressed: configuration.isPressed), in: shape)
            .overlay(shape.strokeBorder(kind == .secondary ? Color.primary.opacity(0.12) : .clear))
            .foregroundStyle(kind == .primary ? Color.white : Color.primary)
            .contentShape(shape)
    }

    private func background(pressed: Bool) -> Color {
        switch kind {
        case .primary:   return Color.blue.opacity(pressed ? 0.8 : 1)
        case .secondary: return Color.primary.opacity(pressed ? 0.14 : 0.08)
        }
    }
}
