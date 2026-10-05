plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.plugin.compose")
}

// Signing key from ~/.gradle/gradle.properties
val keystore = providers.gradleProperty("HADAL_KEYSTORE").orNull
// Version from the release tag, x.y.z
val version = providers.gradleProperty("HADAL_VERSION").orNull ?: "3.0.0"
val parts = version.split('.').map { it.toInt() }

android {
    namespace = "dev.hadal"
    compileSdk {
        version = release(36) { minorApiLevel = 1 }
    }
    defaultConfig {
        applicationId = "dev.hadal"
        minSdk = 29
        targetSdk = 36
        versionCode = parts[0] * 10000 + parts.getOrElse(1) { 0 } * 100 + parts.getOrElse(2) { 0 }
        versionName = version
    }
    signingConfigs {
        if (keystore != null) create("release") {
            storeFile = file(keystore)
            storePassword = providers.gradleProperty("HADAL_KEY_PASSWORD").get()
            keyAlias = "hadal"
            keyPassword = storePassword
        }
    }
    buildTypes {
        release {
            isMinifyEnabled = true
            isShrinkResources = true
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"), "proguard-rules.pro")
            signingConfig = signingConfigs.findByName("release")
        }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    buildFeatures { compose = true }
}

dependencies {
    implementation(platform("androidx.compose:compose-bom:2026.02.01"))
    implementation("androidx.compose.material3:material3")
    implementation("androidx.activity:activity-compose:1.8.2")
    implementation("androidx.lifecycle:lifecycle-runtime-compose:2.9.4")
    implementation("com.google.android.gms:play-services-code-scanner:16.1.0")
}
