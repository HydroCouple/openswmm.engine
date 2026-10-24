# D1 falsifier: a planted SDK dependency. The guard MUST fail on this file.
find_package(HydroCoupleSDK CONFIG REQUIRED)
target_link_libraries(planted_target PRIVATE HydroCoupleSDK::HydroCoupleSDK)
