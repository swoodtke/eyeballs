import SwiftUI

/// Shared circle-indicator language: green = full, orange = partial,
/// white with a black border = empty.
enum IndicatorFill {
    case green, orange, empty
}

/// Fixed width for the numeric value column beside indicator circles, so
/// the circles align across rows and don't shift as the text changes.
enum IndicatorLayout {
    static let valueWidth: CGFloat = 76
}

struct IndicatorCircle: View {
    let fill: IndicatorFill

    var body: some View {
        Circle()
            .fill(color)
            .overlay(Circle().strokeBorder(fill == .empty ? Color.black : .clear,
                                           lineWidth: 1))
            .frame(width: 10, height: 10)
    }

    private var color: Color {
        switch fill {
        case .green:  return .green
        case .orange: return .orange
        case .empty:  return .white
        }
    }
}

/// Proximity as 3 circles: green for each level of signal (3 = close,
/// 2 = near, 1 = far), empty otherwise.
struct SignalCirclesView: View {
    let bars: Int   // 0–3

    var body: some View {
        HStack(spacing: 3) {
            ForEach(0..<3, id: \.self) { i in
                IndicatorCircle(fill: i < bars ? .green : .empty)
            }
        }
        .accessibilityLabel(bars == 0 ? "No signal reading"
                            : ["Far", "Near", "Close"][bars - 1])
    }
}

/// Battery as 3 circles, one per third of charge: green when that third
/// is full, orange while it's the third being drained, empty below it.
/// 10% -> orange/empty/empty; 90% -> green/green/orange.
struct BatteryCirclesView: View {
    let percent: Double

    var body: some View {
        HStack(spacing: 3) {
            ForEach(0..<3, id: \.self) { i in
                IndicatorCircle(fill: fill(for: i))
            }
        }
        .accessibilityLabel(String(format: "Battery %.0f%%", percent))
    }

    private func fill(for third: Int) -> IndicatorFill {
        let low = Double(third) * 100.0 / 3.0
        let high = Double(third + 1) * 100.0 / 3.0
        if percent >= high - 0.5 { return .green }
        if percent > low + 0.5 { return .orange }
        return .empty
    }
}
