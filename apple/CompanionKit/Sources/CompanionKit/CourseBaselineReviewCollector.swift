import Foundation

public enum CourseBaselineReviewCollectionError: Error, Equatable, Sendable {
    case busy, requestIDsExhausted
}

public actor CourseBaselineReviewCollector {
    private var busy = false
    private var requestID: UInt32 = 0
    public init() {}

    public func collect(session: AuthenticatedReaderSession, course: Data) async throws -> CourseBaselineReview {
        try await collect(device: session.device, transport: session, course: course)
    }

    func collect(device: DeviceDescriptor, transport: any CompanionTransport, course: Data,
                 pageLimit: Int = CourseBaselineReviewPage.maximumBytes) async throws -> CourseBaselineReview {
        guard device.readerCapabilities.supportsCourseBaselineReview else {
            throw CourseBaselineReviewControlError.unsupported
        }
        guard !busy else { throw CourseBaselineReviewCollectionError.busy }
        busy = true
        defer { busy = false }
        var assembly = try CourseBaselineReviewAssembly(reader: device.identity, generation: device.storageGeneration,
                                                        course: course)
        while true {
            try Task.checkCancellation()
            guard requestID < UInt32.max else { throw CourseBaselineReviewCollectionError.requestIDsExhausted }
            requestID += 1
            let request = try assembly.nextRequest(limit: pageLimit).frame(requestID: requestID)
            let response = try await transport.exchange(request)
            _ = try CourseBaselineReviewPage.decode(response, request: request)
            try Task.checkCancellation()
            if let review = try assembly.append(response.payload) { return review }
        }
    }
}
