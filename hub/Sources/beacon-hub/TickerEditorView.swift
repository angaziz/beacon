import SwiftUI
import BeaconHubKit

// Menubar ticker editor (issue #92 B4): search Binance + Yahoo, curate an ordered desired list (<= 16),
// and persist+push on every edit via the B3 path (model.onApplyTickerEdit). All search/merge/encoding
// logic is the tested B1/B2 layer; this view only debounces, displays, and mutates a local working copy.
// Shares HubViewModel with the popover panel so the sync badge reflects the live config_ack.

// Firmware bound (records.h FIN_MAX / MAX_TICKERS). Mirrors the contract; the device rejects a longer list.
private let maxTickers = 16

struct TickerEditorView: View {
    @ObservedObject var model: HubViewModel

    @State private var query = ""
    @State private var results: [TickerCandidate] = []
    @State private var working: [TickerRow] = []
    @State private var searchTask: Task<Void, Never>?
    // Per-row add gating (issue #92): the row whose test-fetch is in flight, and the last failure reason
    // keyed by row id (cleared when that row is retried). Keeps add async without blocking the UI.
    @State private var validatingID: String?
    @State private var addErrors: [String: String] = [:]

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            header
            searchModule
            resultsModule
            currentListModule
        }
        .padding(12)
        .frame(width: 420, height: 520, alignment: .top)
        // Seed from the persisted list; re-seed if it changes underneath us (e.g. a push edit elsewhere).
        .onAppear { working = model.tickerRows }
        .onChange(of: model.tickerRows) { working = $0 }
    }

    // MARK: - Header / sync badge

    private var header: some View {
        HStack {
            Text("Tickers").font(.system(size: 15, weight: .semibold))
            Spacer()
            SyncBadge(status: model.tickerSync)
        }
    }

    // MARK: - Search

    private var searchModule: some View {
        Module {
            HStack(spacing: 8) {
                Image(systemName: "magnifyingglass").foregroundStyle(.secondary).font(.system(size: 12))
                TextField("Search Binance + Yahoo", text: $query)
                    .textFieldStyle(.plain)
                    .font(.system(size: 13))
                    .onChange(of: query) { runSearch($0) }
                if !query.isEmpty {
                    Button { query = ""; results = [] } label: {
                        Image(systemName: "xmark.circle.fill").foregroundStyle(.secondary)
                    }
                    .buttonStyle(.plain)
                }
            }
        }
    }

    // Debounce ~300ms: cancel the prior task, sleep, then call the merged search hook. A cancelled sleep
    // throws, which silently ends the task (no stale query fires).
    private func runSearch(_ text: String) {
        searchTask?.cancel()
        let trimmed = text.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty else { results = []; return }
        searchTask = Task {
            try? await Task.sleep(nanoseconds: 300_000_000)
            guard !Task.isCancelled else { return }
            model.onSearchTickers?(trimmed) { merged in
                Task { @MainActor in
                    guard !Task.isCancelled else { return }
                    results = merged
                }
            }
        }
    }

    // MARK: - Results

    private var resultsModule: some View {
        Module(padding: 0) {
            ScrollView {
                LazyVStack(spacing: 0) {
                    if results.isEmpty {
                        Text(query.isEmpty ? "Type to search symbols." : "No matches.")
                            .font(.system(size: 11)).foregroundStyle(.secondary)
                            .frame(maxWidth: .infinity, alignment: .leading)
                            .padding(11)
                    } else {
                        ForEach(results, id: \.row.id) { candidate in
                            ResultRow(candidate: candidate,
                                      canAdd: working.count < maxTickers && !working.contains { $0.id == candidate.row.id },
                                      validating: validatingID == candidate.row.id,
                                      error: addErrors[candidate.row.id],
                                      add: { add(candidate.row) })
                            Divider().padding(.leading, 11)
                        }
                    }
                }
            }
        }
        .frame(maxHeight: 200)
    }

    // MARK: - Current list

    private var currentListModule: some View {
        Module(padding: 0) {
            VStack(spacing: 0) {
                HStack {
                    Text("Current list").font(.system(size: 12, weight: .semibold))
                    Spacer()
                    Text("\(working.count) / \(maxTickers)")
                        .font(.system(size: 11))
                        .foregroundStyle(working.count >= maxTickers ? .orange : .secondary)
                }
                .padding(.horizontal, 11).padding(.top, 11).padding(.bottom, 6)

                if working.isEmpty {
                    Text("Add symbols from search above.")
                        .font(.system(size: 11)).foregroundStyle(.secondary)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .padding(.horizontal, 11).padding(.bottom, 11)
                } else {
                    ScrollView {
                        LazyVStack(spacing: 0) {
                            ForEach(Array(working.enumerated()), id: \.element.id) { idx, row in
                                CurrentRow(row: row,
                                           canMoveUp: idx > 0,
                                           canMoveDown: idx < working.count - 1,
                                           moveUp: { move(idx, by: -1) },
                                           moveDown: { move(idx, by: 1) },
                                           remove: { remove(row) })
                                Divider().padding(.leading, 11)
                            }
                        }
                    }
                }
            }
        }
    }

    // MARK: - Mutations (each commits via B3: persist + push)

    // Test-fetch the device's data endpoint before adding (issue #92): only a row that returns live data
    // joins the list. While validating, the row's Add button shows a spinner; on failure the reason is
    // surfaced inline and the row is NOT added. Ignore re-taps while a validation is already in flight.
    private func add(_ row: TickerRow) {
        guard working.count < maxTickers, !working.contains(where: { $0.id == row.id }), validatingID == nil
        else { return }
        addErrors[row.id] = nil
        guard let validate = model.onValidateTicker else {
            working.append(row); commit(); return   // no hook (bare dev build): keep the prior add behavior
        }
        validatingID = row.id
        validate(row) { ok, reason in
            validatingID = nil
            guard ok else { addErrors[row.id] = reason ?? "No live data for \(row.sym)"; return }
            guard working.count < maxTickers, !working.contains(where: { $0.id == row.id }) else { return }
            working.append(row)
            commit()
        }
    }

    private func remove(_ row: TickerRow) {
        working.removeAll { $0.id == row.id }
        commit()
    }

    private func move(_ index: Int, by offset: Int) {
        let target = index + offset
        guard working.indices.contains(index), working.indices.contains(target) else { return }
        working.swapAt(index, target)
        commit()
    }

    private func commit() { model.onApplyTickerEdit(working) }
}

// MARK: - Rows

private struct ResultRow: View {
    let candidate: TickerCandidate
    let canAdd: Bool
    let validating: Bool
    let error: String?
    let add: () -> Void

    var body: some View {
        VStack(alignment: .leading, spacing: 3) {
            HStack(spacing: 8) {
                VStack(alignment: .leading, spacing: 2) {
                    Text(candidate.row.name).font(.system(size: 12, weight: .medium)).lineLimit(1)
                    HStack(spacing: 6) {
                        Text(candidate.row.sym).font(.system(size: 10)).foregroundStyle(.secondary).lineLimit(1)
                        SourceChip(label: candidate.sourceLabel)
                    }
                }
                Spacer(minLength: 6)
                if validating {
                    ProgressView().controlSize(.small)
                } else {
                    DeckButton(title: "Add", kind: .primary, enabled: canAdd, action: add)
                }
            }
            if let error {
                Text(error).font(.system(size: 10)).foregroundStyle(.red).lineLimit(2)
            }
        }
        .padding(.horizontal, 11).padding(.vertical, 8)
    }
}

private struct CurrentRow: View {
    let row: TickerRow
    let canMoveUp: Bool
    let canMoveDown: Bool
    let moveUp: () -> Void
    let moveDown: () -> Void
    let remove: () -> Void

    var body: some View {
        HStack(spacing: 8) {
            VStack(alignment: .leading, spacing: 2) {
                Text(row.name).font(.system(size: 12, weight: .medium)).lineLimit(1)
                Text(row.sym).font(.system(size: 10)).foregroundStyle(.secondary).lineLimit(1)
            }
            Spacer(minLength: 6)
            IconButton(systemImage: "chevron.up", enabled: canMoveUp, action: moveUp)
            IconButton(systemImage: "chevron.down", enabled: canMoveDown, action: moveDown)
            IconButton(systemImage: "trash", tint: .red, enabled: true, action: remove)
        }
        .padding(.horizontal, 11).padding(.vertical, 8)
    }
}

private struct IconButton: View {
    let systemImage: String
    var tint: Color = .secondary
    let enabled: Bool
    let action: () -> Void

    var body: some View {
        Button(action: action) {
            Image(systemName: systemImage).font(.system(size: 12)).frame(width: 22, height: 22)
        }
        .buttonStyle(.plain)
        .foregroundStyle(tint)
        .disabled(!enabled)
        .opacity(enabled ? 1 : 0.3)
    }
}

private struct SourceChip: View {
    let label: String
    var body: some View {
        Text(label)
            .font(.system(size: 9, weight: .semibold))
            .padding(.horizontal, 5).padding(.vertical, 1)
            .background(.primary.opacity(0.1), in: Capsule())
            .foregroundStyle(.secondary)
    }
}

private struct SyncBadge: View {
    let status: TickerSyncStatus
    var body: some View {
        Label(text, systemImage: icon)
            .font(.system(size: 11, weight: .medium))
            .foregroundStyle(color)
    }
    private var text: String {
        switch status {
        case .idle:            return "Not synced"
        case .pending:         return "Syncing…"
        case .synced(let n):   return "Synced \(n)"
        case .error(let msg):  return "Error: \(msg)"
        }
    }
    private var icon: String {
        switch status {
        case .idle:    return "circle"
        case .pending: return "arrow.triangle.2.circlepath"
        case .synced:  return "checkmark.circle.fill"
        case .error:   return "exclamationmark.triangle.fill"
        }
    }
    private var color: Color {
        switch status {
        case .idle:    return .secondary
        case .pending: return .secondary
        case .synced:  return .green
        case .error:   return .red
        }
    }
}
