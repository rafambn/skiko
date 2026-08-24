import org.jetbrains.kotlin.gradle.dsl.JvmTarget

val kspVersion: String by project

plugins {
    kotlin("multiplatform")
}

repositories {
    mavenCentral {
        url = uri("https://cache-redirector.jetbrains.com/maven-central")
    }
}

kotlin {
    jvm {
        compilerOptions.jvmTarget.set(JvmTarget.JVM_11)
    }
    sourceSets {
        val jvmMain by getting {
            dependencies {
                compileOnly(kotlin("compiler-embeddable"))
            }
        }
    }
}
