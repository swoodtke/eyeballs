// swift-tools-version: 5.9
import PackageDescription

let package = Package(
    name: "EyeballController",
    platforms: [.macOS(.v13)],
    products: [
        .library(name: "EyeballBLE", targets: ["EyeballBLE"]),
    ],
    targets: [
        .target(name: "EyeballBLE"),
        .executableTarget(
            name: "EyeballApp",
            dependencies: ["EyeballBLE"],
            exclude: ["Info.plist"]
        ),
    ]
)
