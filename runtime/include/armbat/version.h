#pragma once

namespace armbat {

// The version the APK was built with. It comes from app/build.gradle.kts through
// a generated header, so the runtime never keeps a second copy of the number.
const char* VersionString();

}  // namespace armbat
