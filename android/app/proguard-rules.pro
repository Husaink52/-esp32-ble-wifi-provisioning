# =============================================================================
# proguard-rules.pro: R8 keep rules for release builds
#
# Not active yet: isMinifyEnabled = false in app/build.gradle.kts (v1).
# The rules below are ready for when shrinking is turned on. Test a release
# build on a real device before shipping.
# =============================================================================

# EventBus finds @Subscribe methods by reflection, so R8 must not rename or remove them.
-keepattributes *Annotation*
-keepclassmembers class * {
    @org.greenrobot.eventbus.Subscribe <methods>;
}
-keep enum org.greenrobot.eventbus.ThreadMode { *; }

# The Espressif library's protobuf-lite message classes use reflection on field names.
-keep class * extends com.google.protobuf.GeneratedMessageLite { *; }
-keep class com.espressif.provisioning.** { *; }
