// swift-tools-version: 5.9
import PackageDescription

let package = Package(
    name: "EyeballController",
    platforms: [.macOS(.v13), .iOS(.v16)],
    products: [
        .library(name: "EyeballBLE", targets: ["EyeballBLE"]),
        .library(name: "EyeballUI", targets: ["EyeballUI"]),
    ],
    targets: [
        .target(name: "EyeballBLE"),
        .target(name: "EyeballUI", dependencies: ["EyeballBLE"]),
        .executableTarget(
            name: "EyeballApp",
            dependencies: ["EyeballBLE", "EyeballUI"],
            exclude: ["Info.plist"]
        ),
    ]
)
