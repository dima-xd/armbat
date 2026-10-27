package dev.armbat;

import android.content.pm.ApplicationInfo;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.os.Bundle;

/**
 * The package manager a guest sees.
 *
 * A guest has no installed package: every query would be answered
 * from the container's own manifest, so an SDK that reads its
 * application's <meta-data> or its version gets nothing and dies on the
 * first lookup. This wrapper answers those few queries from the archive's
 * manifest and delegates everything else to the platform.
 *
 * The delegating methods (PackageManager has about ninety abstract methods and
 * none of them matter here) are kept in sync with the platform class by hand.
 */
final class GuestPackageManager extends PackageManager {
    private final PackageManager base;
    private final ApplicationInfo archive;
    private final String archivePath;
    private final String nativeLibraryDir;
    private final String hostPackage;
    private final String[] requestedPermissions;
    private volatile java.util.Map<String, android.content.pm.ActivityInfo> archiveActivities;

    GuestPackageManager(PackageManager base, ApplicationInfo archive, String archivePath,
                        String nativeLibraryDir, String hostPackage) {
        this.base = base;
        this.archive = archive;
        this.archivePath = archivePath;
        this.nativeLibraryDir = nativeLibraryDir;
        this.hostPackage = hostPackage;
        // The permissions the guest's own manifest asks for. The container
        // carries a pool of them, because an archive cannot be installed and the
        // runtime ones have to be held by this process; a guest that reads its
        // package, however, has to see the manifest it was built with, or an SDK
        // takes a path for a permission the application never asked for.
        String[] permissions = null;
        try {
            @SuppressWarnings("deprecation")
            PackageInfo archived =
                    base.getPackageArchiveInfo(archivePath, PackageManager.GET_PERMISSIONS);
            if (archived != null) {
                permissions = archived.requestedPermissions;
            }
        } catch (RuntimeException ignored) {
            // An archive the platform cannot parse keeps the container's answer.
        }
        this.requestedPermissions = permissions;
    }

    /**
     * The archive's application info with the container's identity: the guest
     * keeps its data directory and its package name, but sees the manifest it
     * was built with.
     *
     * The presentation fields (label, icon, theme, description) have to come
     * from the archive as well. They are resource ids, and a guest resolves
     * them against the archive's resource table; the container's ids point
     * into the container's table, where an archive's Context finds nothing.
     * An application that prints its own label during startup -- EA's
     * downloader does -- dies on a Resources$NotFoundException otherwise.
     */
    private ApplicationInfo merged(ApplicationInfo info, int flags) {
        if (info == null) {
            return null;
        }
        ApplicationInfo copy = new ApplicationInfo(info);
        if (archive != null) {
            if ((flags & PackageManager.GET_META_DATA) != 0 && archive.metaData != null) {
                copy.metaData = new Bundle(archive.metaData);
            }
            copy.labelRes = archive.labelRes;
            copy.nonLocalizedLabel = archive.nonLocalizedLabel;
            copy.icon = archive.icon;
            copy.logo = archive.logo;
            copy.banner = archive.banner;
            copy.descriptionRes = archive.descriptionRes;
            copy.theme = archive.theme;
            copy.sourceDir = archivePath;
            copy.publicSourceDir = archivePath;
            if (nativeLibraryDir != null) {
                copy.nativeLibraryDir = nativeLibraryDir;
            }
        }
        return copy;
    }

    /**
     * Packages a device without Google services does not have. Guests probe for
     * the Play Store and for Play services to decide which store or licence path
     * to take; answering from the container's phone would hand them a billing
     * service they cannot use.
     *
     * The answer has to be the same everywhere the platform is asked about a
     * package: the package manager, the store binding, and the context a guest
     * builds for another package. The Google+ view an old game may bundle asks
     * for the Play services context to read its own icons and, when the context
     * exists but the icon does not, calls Resources.getDrawable(0) and takes the
     * process down with the exception.
     */
    private static final String[] ABSENT_PACKAGES = {
            "com.android.vending",
            "com.google.android.gms",
            "com.google.android.gsf",
            "com.google.android.gsf.login",
    };

    static boolean isAbsentPackage(String packageName) {
        for (String absent : ABSENT_PACKAGES) {
            if (absent.equals(packageName)) {
                return true;
            }
        }
        return false;
    }

    private android.content.pm.ActivityInfo explicitArchiveActivity(android.content.Intent intent,
                                                                int flags) {
        if (intent == null) {
            return null;
        }
        android.content.ComponentName component = intent.getComponent();
        return component != null ? archiveActivity(component, flags) : null;
    }

    /**
     * The package name the guest believes it has: the archive's, not the
     * container's. A game names its own activities with it when it builds a
     * ComponentName, so a query about "my activity" arrives with this package,
     * never with the container's.
     */
    private String guestPackage() {
        return archive != null ? archive.packageName : hostPackage;
    }

    /**
     * An activity the archive's manifest declares.
     *
     * A library asks the platform whether the host application declared the
     * component it needs -- AdMob refuses to show an ad until it finds its own
     * {@code com.google.ads.AdActivity} and checks its configChanges. The
     * archive is not installed, so the platform only knows the container's
     * activities; the answer has to come from the archive's manifest, under the
     * package name the guest believes it has.
     */
    private android.content.pm.ActivityInfo archiveActivity(android.content.ComponentName component,
                                                            int flags) {
        if (archive == null || archivePath == null || component == null
                || !guestPackage().equals(component.getPackageName())) {
            return null;
        }
        java.util.Map<String, android.content.pm.ActivityInfo> activities = archiveActivities;
        if (activities == null) {
            synchronized (this) {
                activities = archiveActivities;
                if (activities == null) {
                    activities = new java.util.HashMap<>();
                    try {
                        @SuppressWarnings("deprecation")
                        PackageInfo archived = base.getPackageArchiveInfo(
                                archivePath,
                                PackageManager.GET_ACTIVITIES | PackageManager.GET_META_DATA);
                        if (archived != null && archived.activities != null) {
                            for (android.content.pm.ActivityInfo activity : archived.activities) {
                                activities.put(activity.name, activity);
                            }
                        }
                    } catch (RuntimeException ignored) {
                        // An archive the platform cannot parse declares nothing.
                    }
                    archiveActivities = activities;
                }
            }
        }
        android.content.pm.ActivityInfo found = activities.get(component.getClassName());
        if (found == null) {
            return null;
        }
        android.content.pm.ActivityInfo copy = new android.content.pm.ActivityInfo(found);
        copy.packageName = guestPackage();
        copy.applicationInfo =
                merged(found.applicationInfo != null ? found.applicationInfo : archive, flags);
        return copy;
    }

    /**
     * Whether a query names the archive's own package, the name its manifest
     * declares.
     */
    private boolean isArchivePackage(String packageName) {
        return archive != null && packageName != null && packageName.equals(archive.packageName);
    }

    /**
     * The archive as the installed package it believes it is: its own manifest
     * entry, its paths, and the identity of the process that runs it.
     */
    private ApplicationInfo archiveApplicationInfo(int flags)
            throws NameNotFoundException {
        ApplicationInfo copy = new ApplicationInfo(archive);
        ApplicationInfo installed = base.getApplicationInfo(hostPackage, flags);
        copy.uid = installed.uid;
        copy.dataDir = installed.dataDir;
        copy.sourceDir = archivePath;
        copy.publicSourceDir = archivePath;
        if (nativeLibraryDir != null) {
            copy.nativeLibraryDir = nativeLibraryDir;
        }
        if ((flags & PackageManager.GET_META_DATA) == 0) {
            copy.metaData = null;
        }
        return copy;
    }

    @Override
    public ApplicationInfo getApplicationInfo(String packageName, int flags)
            throws NameNotFoundException {
        if (isAbsentPackage(packageName)) {
            throw new NameNotFoundException(packageName + " is not installed");
        }
        if (isArchivePackage(packageName)) {
            return archiveApplicationInfo(flags);
        }
        return merged(base.getApplicationInfo(packageName, flags), flags);
    }

    @Override
    public PackageInfo getPackageInfo(String packageName, int flags)
            throws NameNotFoundException {
        if (isAbsentPackage(packageName)) {
            throw new NameNotFoundException(packageName + " is not installed");
        }
        if (isArchivePackage(packageName)) {
            PackageInfo info = base.getPackageInfo(hostPackage, flags);
            info.packageName = packageName;
            info.applicationInfo = archiveApplicationInfo(flags);
            // The version is the archive's, not the container's: a guest reads
            // it to decide which of its own code paths to take.
            PackageInfo archived = null;
            try {
                archived = base.getPackageArchiveInfo(archivePath, flags);
            } catch (RuntimeException ignored) {
                // An archive the platform cannot parse keeps the container's answer.
            }
            if (archived != null) {
                info.versionCode = archived.versionCode;
                info.versionName = archived.versionName;
            }
            if (requestedPermissions != null) {
                info.requestedPermissions = requestedPermissions.clone();
                info.requestedPermissionsFlags = grantedFlags(requestedPermissions, hostPackage);
            }
            return info;
        }
        PackageInfo info = base.getPackageInfo(packageName, flags);
        if (info != null && archive != null) {
            info.applicationInfo = merged(info.applicationInfo, flags);
        }
        // Only the package the guest believes it is: a query about another
        // package has to keep the platform's own answer.
        if (info != null && requestedPermissions != null && isArchivePackage(packageName)) {
            info.requestedPermissions = requestedPermissions.clone();
            info.requestedPermissionsFlags = grantedFlags(requestedPermissions, hostPackage);
        }
        return info;
    }

    /**
     * The flags of the archive's permissions as this process holds them: a
     * normal permission is granted at install, a runtime one only after the
     * container asked for it before the guest started.
     */
    private int[] grantedFlags(String[] permissions, String packageName) {
        int[] granted = new int[permissions.length];
        for (int index = 0; index < permissions.length; ++index) {
            if (base.checkPermission(permissions[index], packageName)
                    == PackageManager.PERMISSION_GRANTED) {
                granted[index] = PackageInfo.REQUESTED_PERMISSION_GRANTED;
            }
        }
        return granted;
    }

    @Override
    public void addPackageToPreferred(java.lang.String arg0) {
        base.addPackageToPreferred(arg0);
    }

    @Override
    public boolean addPermission(android.content.pm.PermissionInfo arg0) {
        return base.addPermission(arg0);
    }

    @Override
    public boolean addPermissionAsync(android.content.pm.PermissionInfo arg0) {
        return base.addPermissionAsync(arg0);
    }

    @Override
    public void addPreferredActivity(android.content.IntentFilter arg0, int arg1, android.content.ComponentName[] arg2, android.content.ComponentName arg3) {
        base.addPreferredActivity(arg0, arg1, arg2, arg3);
    }

    @Override
    public boolean canRequestPackageInstalls() {
        return base.canRequestPackageInstalls();
    }

    @Override
    public java.lang.String[] canonicalToCurrentPackageNames(java.lang.String[] arg0) {
        return base.canonicalToCurrentPackageNames(arg0);
    }

    @Override
    public int checkPermission(java.lang.String arg0, java.lang.String arg1) {
        if (holdsLegacyStorage(arg1)) {
            return PackageManager.PERMISSION_GRANTED;
        }
        return base.checkPermission(arg0, arg1);
    }

    /**
     * Whether a permission that the platform no longer grants is nevertheless
     * satisfied here.
     *
     * READ/WRITE_EXTERNAL_STORAGE cannot be held by an application on a current
     * platform: the permission is capped by target SDK and the platform answers
     * "denied" no matter what the user chose. An archive of the Android 4 era
     * asks for it before it starts its engine and stops at its own "grant the
     * permission" screen otherwise, so the guest has to see the access it
     * actually has. The container runs only with "all files access", which is a
     * superset of what those permissions ever gave, so it is that grant the
     * guest is answered from.
     */
    private boolean holdsLegacyStorage(String packageName) {
        // The guest asks for its own package, which is the archive's name; the
        // container's name is accepted as well because some libraries query the
        // process they run in.
        if (!isArchivePackage(packageName) && !hostPackage.equals(packageName)) {
            return false;
        }
        return android.os.Environment.isExternalStorageManager();
    }

    /**
     * The rationale query is abstract on the platform's PackageManager but
     * hidden from the public SDK, so this wrapper compiled against the SDK has
     * no slot for it: every Activity.shouldShowRequestPermissionRationale call
     * from a guest dies with AbstractMethodError. Ask the platform through
     * reflection when the hidden API is reachable, and answer "nothing to
     * explain" otherwise -- the guest goes on to request the permission, which
     * is the branch its own code takes for a first request anyway.
     */
    public boolean shouldShowRequestPermissionRationale(String permission) {
        try {
            java.lang.reflect.Method method = PackageManager.class.getMethod(
                    "shouldShowRequestPermissionRationale", String.class);
            Object answer = method.invoke(base, permission);
            return answer instanceof Boolean && (Boolean) answer;
        } catch (ReflectiveOperationException | RuntimeException unreachable) {
            return false;
        }
    }

    @Override
    public int checkSignatures(int arg0, int arg1) {
        return base.checkSignatures(arg0, arg1);
    }

    @Override
    public int checkSignatures(java.lang.String arg0, java.lang.String arg1) {
        return base.checkSignatures(arg0, arg1);
    }

    @Override
    public void clearInstantAppCookie() {
        base.clearInstantAppCookie();
    }

    @Override
    public void clearPackagePreferredActivities(java.lang.String arg0) {
        base.clearPackagePreferredActivities(arg0);
    }

    @Override
    public java.lang.String[] currentToCanonicalPackageNames(java.lang.String[] arg0) {
        return base.currentToCanonicalPackageNames(arg0);
    }

    @Override
    public void extendVerificationTimeout(int arg0, int arg1, long arg2) {
        base.extendVerificationTimeout(arg0, arg1, arg2);
    }

    @Override
    public android.graphics.drawable.Drawable getActivityBanner(android.content.ComponentName arg0) throws android.content.pm.PackageManager.NameNotFoundException {
        return base.getActivityBanner(arg0);
    }

    @Override
    public android.graphics.drawable.Drawable getActivityBanner(android.content.Intent arg0) throws android.content.pm.PackageManager.NameNotFoundException {
        return base.getActivityBanner(arg0);
    }

    @Override
    public android.graphics.drawable.Drawable getActivityIcon(android.content.ComponentName arg0) throws android.content.pm.PackageManager.NameNotFoundException {
        return base.getActivityIcon(arg0);
    }

    @Override
    public android.graphics.drawable.Drawable getActivityIcon(android.content.Intent arg0) throws android.content.pm.PackageManager.NameNotFoundException {
        return base.getActivityIcon(arg0);
    }

    @Override
    public android.content.pm.ActivityInfo getActivityInfo(android.content.ComponentName arg0, int arg1) throws android.content.pm.PackageManager.NameNotFoundException {
        android.content.pm.ActivityInfo declared = archiveActivity(arg0, arg1);
        if (declared != null) {
            return declared;
        }
        return base.getActivityInfo(arg0, arg1);
    }

    @Override
    public android.graphics.drawable.Drawable getActivityLogo(android.content.ComponentName arg0) throws android.content.pm.PackageManager.NameNotFoundException {
        return base.getActivityLogo(arg0);
    }

    @Override
    public android.graphics.drawable.Drawable getActivityLogo(android.content.Intent arg0) throws android.content.pm.PackageManager.NameNotFoundException {
        return base.getActivityLogo(arg0);
    }

    @Override
    public java.util.List<android.content.pm.PermissionGroupInfo> getAllPermissionGroups(int arg0) {
        return base.getAllPermissionGroups(arg0);
    }

    @Override
    public android.graphics.drawable.Drawable getApplicationBanner(android.content.pm.ApplicationInfo arg0) {
        return base.getApplicationBanner(arg0);
    }

    @Override
    public android.graphics.drawable.Drawable getApplicationBanner(java.lang.String arg0) throws android.content.pm.PackageManager.NameNotFoundException {
        return base.getApplicationBanner(arg0);
    }

    @Override
    public int getApplicationEnabledSetting(java.lang.String arg0) {
        return base.getApplicationEnabledSetting(arg0);
    }

    @Override
    public android.graphics.drawable.Drawable getApplicationIcon(android.content.pm.ApplicationInfo arg0) {
        return base.getApplicationIcon(arg0);
    }

    @Override
    public android.graphics.drawable.Drawable getApplicationIcon(java.lang.String arg0) throws android.content.pm.PackageManager.NameNotFoundException {
        return base.getApplicationIcon(arg0);
    }

    @Override
    public java.lang.CharSequence getApplicationLabel(android.content.pm.ApplicationInfo arg0) {
        return base.getApplicationLabel(arg0);
    }

    @Override
    public android.graphics.drawable.Drawable getApplicationLogo(android.content.pm.ApplicationInfo arg0) {
        return base.getApplicationLogo(arg0);
    }

    @Override
    public android.graphics.drawable.Drawable getApplicationLogo(java.lang.String arg0) throws android.content.pm.PackageManager.NameNotFoundException {
        return base.getApplicationLogo(arg0);
    }

    @Override
    public android.content.pm.ChangedPackages getChangedPackages(int arg0) {
        return base.getChangedPackages(arg0);
    }

    @Override
    public int getComponentEnabledSetting(android.content.ComponentName arg0) {
        return base.getComponentEnabledSetting(arg0);
    }

    @Override
    public android.graphics.drawable.Drawable getDefaultActivityIcon() {
        return base.getDefaultActivityIcon();
    }

    @Override
    public android.graphics.drawable.Drawable getDrawable(java.lang.String arg0, int arg1, android.content.pm.ApplicationInfo arg2) {
        return base.getDrawable(arg0, arg1, arg2);
    }

    @Override
    public java.util.List<android.content.pm.ApplicationInfo> getInstalledApplications(int arg0) {
        return base.getInstalledApplications(arg0);
    }

    @Override
    public java.util.List<android.content.pm.PackageInfo> getInstalledPackages(int arg0) {
        return base.getInstalledPackages(arg0);
    }

    @Override
    public java.lang.String getInstallerPackageName(java.lang.String arg0) {
        return base.getInstallerPackageName(arg0);
    }

    @Override
    public byte[] getInstantAppCookie() {
        return base.getInstantAppCookie();
    }

    @Override
    public int getInstantAppCookieMaxBytes() {
        return base.getInstantAppCookieMaxBytes();
    }

    @Override
    public android.content.pm.InstrumentationInfo getInstrumentationInfo(android.content.ComponentName arg0, int arg1) throws android.content.pm.PackageManager.NameNotFoundException {
        return base.getInstrumentationInfo(arg0, arg1);
    }

    @Override
    public android.content.Intent getLaunchIntentForPackage(java.lang.String arg0) {
        return base.getLaunchIntentForPackage(arg0);
    }

    @Override
    public android.content.Intent getLeanbackLaunchIntentForPackage(java.lang.String arg0) {
        return base.getLeanbackLaunchIntentForPackage(arg0);
    }

    @Override
    public java.lang.String getNameForUid(int arg0) {
        return base.getNameForUid(arg0);
    }

    @Override
    public int[] getPackageGids(java.lang.String arg0) throws android.content.pm.PackageManager.NameNotFoundException {
        return base.getPackageGids(arg0);
    }

    @Override
    public int[] getPackageGids(java.lang.String arg0, int arg1) throws android.content.pm.PackageManager.NameNotFoundException {
        return base.getPackageGids(arg0, arg1);
    }

    @Override
    public android.content.pm.PackageInfo getPackageInfo(android.content.pm.VersionedPackage arg0, int arg1) throws android.content.pm.PackageManager.NameNotFoundException {
        return base.getPackageInfo(arg0, arg1);
    }

    @Override
    public android.content.pm.PackageInstaller getPackageInstaller() {
        return base.getPackageInstaller();
    }

    @Override
    public int getPackageUid(java.lang.String arg0, int arg1) throws android.content.pm.PackageManager.NameNotFoundException {
        return base.getPackageUid(arg0, arg1);
    }

    @Override
    public java.lang.String[] getPackagesForUid(int arg0) {
        return base.getPackagesForUid(arg0);
    }

    @Override
    public java.util.List<android.content.pm.PackageInfo> getPackagesHoldingPermissions(java.lang.String[] arg0, int arg1) {
        return base.getPackagesHoldingPermissions(arg0, arg1);
    }

    @Override
    public android.content.pm.PermissionGroupInfo getPermissionGroupInfo(java.lang.String arg0, int arg1) throws android.content.pm.PackageManager.NameNotFoundException {
        return base.getPermissionGroupInfo(arg0, arg1);
    }

    @Override
    public android.content.pm.PermissionInfo getPermissionInfo(java.lang.String arg0, int arg1) throws android.content.pm.PackageManager.NameNotFoundException {
        return base.getPermissionInfo(arg0, arg1);
    }

    @Override
    public int getPreferredActivities(java.util.List<android.content.IntentFilter> arg0, java.util.List<android.content.ComponentName> arg1, java.lang.String arg2) {
        return base.getPreferredActivities(arg0, arg1, arg2);
    }

    @Override
    public java.util.List<android.content.pm.PackageInfo> getPreferredPackages(int arg0) {
        return base.getPreferredPackages(arg0);
    }

    @Override
    public android.content.pm.ProviderInfo getProviderInfo(android.content.ComponentName arg0, int arg1) throws android.content.pm.PackageManager.NameNotFoundException {
        return base.getProviderInfo(arg0, arg1);
    }

    @Override
    public android.content.pm.ActivityInfo getReceiverInfo(android.content.ComponentName arg0, int arg1) throws android.content.pm.PackageManager.NameNotFoundException {
        return base.getReceiverInfo(arg0, arg1);
    }

    @Override
    public android.content.res.Resources getResourcesForActivity(android.content.ComponentName arg0) throws android.content.pm.PackageManager.NameNotFoundException {
        return base.getResourcesForActivity(arg0);
    }

    @Override
    public android.content.res.Resources getResourcesForApplication(android.content.pm.ApplicationInfo arg0) throws android.content.pm.PackageManager.NameNotFoundException {
        return base.getResourcesForApplication(arg0);
    }

    @Override
    public android.content.res.Resources getResourcesForApplication(java.lang.String arg0) throws android.content.pm.PackageManager.NameNotFoundException {
        return base.getResourcesForApplication(arg0);
    }

    @Override
    public android.content.pm.ServiceInfo getServiceInfo(android.content.ComponentName arg0, int arg1) throws android.content.pm.PackageManager.NameNotFoundException {
        return base.getServiceInfo(arg0, arg1);
    }

    @Override
    public java.util.List<android.content.pm.SharedLibraryInfo> getSharedLibraries(int arg0) {
        return base.getSharedLibraries(arg0);
    }

    @Override
    public android.content.pm.FeatureInfo[] getSystemAvailableFeatures() {
        return base.getSystemAvailableFeatures();
    }

    @Override
    public java.lang.String[] getSystemSharedLibraryNames() {
        return base.getSystemSharedLibraryNames();
    }

    @Override
    public java.lang.CharSequence getText(java.lang.String arg0, int arg1, android.content.pm.ApplicationInfo arg2) {
        return base.getText(arg0, arg1, arg2);
    }

    @Override
    public android.graphics.drawable.Drawable getUserBadgedDrawableForDensity(android.graphics.drawable.Drawable arg0, android.os.UserHandle arg1, android.graphics.Rect arg2, int arg3) {
        return base.getUserBadgedDrawableForDensity(arg0, arg1, arg2, arg3);
    }

    @Override
    public android.graphics.drawable.Drawable getUserBadgedIcon(android.graphics.drawable.Drawable arg0, android.os.UserHandle arg1) {
        return base.getUserBadgedIcon(arg0, arg1);
    }

    @Override
    public java.lang.CharSequence getUserBadgedLabel(java.lang.CharSequence arg0, android.os.UserHandle arg1) {
        return base.getUserBadgedLabel(arg0, arg1);
    }

    @Override
    public android.content.res.XmlResourceParser getXml(java.lang.String arg0, int arg1, android.content.pm.ApplicationInfo arg2) {
        return base.getXml(arg0, arg1, arg2);
    }

    @Override
    public boolean hasSystemFeature(java.lang.String arg0) {
        return base.hasSystemFeature(arg0);
    }

    @Override
    public boolean hasSystemFeature(java.lang.String arg0, int arg1) {
        return base.hasSystemFeature(arg0, arg1);
    }

    @Override
    public boolean isInstantApp() {
        return base.isInstantApp();
    }

    @Override
    public boolean isInstantApp(java.lang.String arg0) {
        return base.isInstantApp(arg0);
    }

    @Override
    public boolean isPermissionRevokedByPolicy(java.lang.String arg0, java.lang.String arg1) {
        return base.isPermissionRevokedByPolicy(arg0, arg1);
    }

    @Override
    public boolean isSafeMode() {
        return base.isSafeMode();
    }

    @Override
    public java.util.List<android.content.pm.ResolveInfo> queryBroadcastReceivers(android.content.Intent arg0, int arg1) {
        return base.queryBroadcastReceivers(arg0, arg1);
    }

    @Override
    public java.util.List<android.content.pm.ProviderInfo> queryContentProviders(java.lang.String arg0, int arg1, int arg2) {
        return base.queryContentProviders(arg0, arg1, arg2);
    }

    @Override
    public java.util.List<android.content.pm.InstrumentationInfo> queryInstrumentation(java.lang.String arg0, int arg1) {
        return base.queryInstrumentation(arg0, arg1);
    }

    @Override
    public java.util.List<android.content.pm.ResolveInfo> queryIntentActivities(android.content.Intent arg0, int arg1) {
        return base.queryIntentActivities(arg0, arg1);
    }

    @Override
    public java.util.List<android.content.pm.ResolveInfo> queryIntentActivityOptions(android.content.ComponentName arg0, android.content.Intent[] arg1, android.content.Intent arg2, int arg3) {
        return base.queryIntentActivityOptions(arg0, arg1, arg2, arg3);
    }

    @Override
    public java.util.List<android.content.pm.ResolveInfo> queryIntentContentProviders(android.content.Intent arg0, int arg1) {
        return base.queryIntentContentProviders(arg0, arg1);
    }

    @Override
    public java.util.List<android.content.pm.ResolveInfo> queryIntentServices(android.content.Intent arg0, int arg1) {
        return base.queryIntentServices(arg0, arg1);
    }

    @Override
    public java.util.List<android.content.pm.PermissionInfo> queryPermissionsByGroup(java.lang.String arg0, int arg1) throws android.content.pm.PackageManager.NameNotFoundException {
        return base.queryPermissionsByGroup(arg0, arg1);
    }

    @Override
    public void removePackageFromPreferred(java.lang.String arg0) {
        base.removePackageFromPreferred(arg0);
    }

    @Override
    public void removePermission(java.lang.String arg0) {
        base.removePermission(arg0);
    }

    @Override
    public android.content.pm.ResolveInfo resolveActivity(android.content.Intent arg0, int arg1) {
        android.content.pm.ActivityInfo declared = explicitArchiveActivity(arg0, arg1);
        if (declared != null) {
            android.content.pm.ResolveInfo info = new android.content.pm.ResolveInfo();
            info.activityInfo = declared;
            info.isDefault = true;
            return info;
        }
        return base.resolveActivity(arg0, arg1);
    }

    @Override
    public android.content.pm.ProviderInfo resolveContentProvider(java.lang.String arg0, int arg1) {
        return base.resolveContentProvider(arg0, arg1);
    }

    @Override
    public android.content.pm.ResolveInfo resolveService(android.content.Intent arg0, int arg1) {
        return base.resolveService(arg0, arg1);
    }

    @Override
    public void setApplicationCategoryHint(java.lang.String arg0, int arg1) {
        base.setApplicationCategoryHint(arg0, arg1);
    }

    @Override
    public void setApplicationEnabledSetting(java.lang.String arg0, int arg1, int arg2) {
        base.setApplicationEnabledSetting(arg0, arg1, arg2);
    }

    @Override
    public void setComponentEnabledSetting(android.content.ComponentName arg0, int arg1, int arg2) {
        base.setComponentEnabledSetting(arg0, arg1, arg2);
    }

    @Override
    public void setInstallerPackageName(java.lang.String arg0, java.lang.String arg1) {
        base.setInstallerPackageName(arg0, arg1);
    }

    @Override
    public void updateInstantAppCookie(byte[] arg0) {
        base.updateInstantAppCookie(arg0);
    }

    @Override
    public void verifyPendingInstall(int arg0, int arg1) {
        base.verifyPendingInstall(arg0, arg1);
    }

}
