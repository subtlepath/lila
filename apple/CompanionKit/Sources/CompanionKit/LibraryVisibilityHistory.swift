import Foundation

public enum LibraryVisibilityError: Error, Equatable, Sendable {
    case invalidChange, equivocation(UUID), cycle, wrongResourceParent
}
public struct LibraryVisibilityChange: Codable, Equatable, Sendable {
    public let version: Int
    public let id: UUID
    public let origin: Data
    public let content: ContentID
    public let removed: Bool
    public let ancestors: [UUID]
    public init(id: UUID = UUID(), origin: Data, content: ContentID, removed: Bool, ancestors: [UUID]) throws {
        version = 1; self.id = id; self.origin = origin; self.content = content; self.removed = removed
        self.ancestors = ancestors.sorted { $0.uuidString < $1.uuidString }
        try validate()
    }
    public func validate() throws {
        guard version == 1, origin.count == 16, origin.contains(where: { $0 != 0 }),
              ancestors.count <= 16, Set(ancestors).count == ancestors.count, !ancestors.contains(id) else {
            throw LibraryVisibilityError.invalidChange
        }
    }
}
public struct LibraryVisibilitySnapshot: Equatable, Sendable {
    public let removed: Bool
    public let heads: [UUID]
    public let deferred: [UUID]
}
public enum LibraryVisibilityHistory {
    // Concurrent removal wins; restoring must observe all effective removal heads.
    public static func merge(_ changes: [LibraryVisibilityChange], content: ContentID,
                             initiallyRemoved: Bool = false) throws -> LibraryVisibilitySnapshot {
        var all: [UUID: LibraryVisibilityChange] = [:]; all.reserveCapacity(changes.count)
        for change in changes {
            try change.validate()
            if let existing = all[change.id], existing != change { throw LibraryVisibilityError.equivocation(change.id) }
            all[change.id] = change
        }
        for change in all.values {
            for parent in change.ancestors {
                if let known = all[parent], known.content != change.content { throw LibraryVisibilityError.wrongResourceParent }
            }
        }
        let selected = all.filter { $0.value.content == content }
        var degrees: [UUID: Int] = [:]; degrees.reserveCapacity(selected.count)
        var children: [UUID: [UUID]] = [:]; children.reserveCapacity(selected.count)
        var ready: [UUID] = []; ready.reserveCapacity(selected.count)
        for change in selected.values {
            var known = 0
            for parent in change.ancestors where selected[parent] != nil {
                known += 1
                children[parent, default: []].append(change.id)
            }
            degrees[change.id] = known
            if known == 0 { ready.append(change.id) }
        }
        var complete: Set<UUID> = []; complete.reserveCapacity(selected.count)
        var covered: Set<UUID> = []; covered.reserveCapacity(selected.count)
        var offset = 0
        while offset < ready.count {
            let id = ready[offset]; offset += 1
            guard let change = selected[id] else { throw LibraryVisibilityError.invalidChange }
            if change.ancestors.allSatisfy({ complete.contains($0) }) {
                complete.insert(id); covered.formUnion(change.ancestors)
            }
            for child in children[id] ?? [] {
                degrees[child, default: 0] -= 1
                if degrees[child] == 0 { ready.append(child) }
            }
        }
        guard ready.count == selected.count else { throw LibraryVisibilityError.cycle }
        let heads = complete.subtracting(covered).sorted { $0.uuidString < $1.uuidString }
        let deferred = Set(selected.keys).subtracting(complete).sorted { $0.uuidString < $1.uuidString }
        return LibraryVisibilitySnapshot(removed: heads.isEmpty ? initiallyRemoved : heads.contains { selected[$0]?.removed == true },
                                         heads: heads, deferred: deferred)
    }
}
