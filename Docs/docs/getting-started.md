Getting Started
===============

What is PresentationTheme Aero?
-------------------------------

_PresentationTheme Aero_ is a highly polished Windows Aero theme for WPF. It
provides faithful pixel-perfect recreation of the native Aero themes including
proper animations depending on system settings, popup drop-shadows,
Explorer-styles. Also includes AeroLite and High Contrast themes.


Installation
------------

[Install the NuGet package](https://docs.microsoft.com/en-us/nuget/tools/package-manager-console#installing-a-package)
`PresentationTheme.Aero`. The package contains the following assemblies:

Assembly                                 | Description
-----------------------------------------|-------------------------------
PresentationTheme.Aero.dll               | Shared assembly with utilities
PresentationTheme.Aero.Win10.dll         | Windows 10 Aero theme
PresentationTheme.AeroLite.Win10.dll     | Windows 10 AeroLite theme
PresentationTheme.HighContrast.Win10.dll | Windows 10 High Contrast theme

Only the first assembly should be referenced. The other theme assemblies are
loaded when necessary.

> [!IMPORTANT]
> Because the theme assemblies are normally not referenced, they have to
> be copied manually to the build output directory.
> 
> A [Post-build event command line](https://docs.microsoft.com/en-us/visualstudio/ide/how-to-specify-build-events-csharp) in Visual Studio can be created to automate 
> this process. The following command is an example of this: 
>
> `for /D %%i in ($(SolutionDir)packages\PresentationTheme.Aero.*) do xcopy /D /Y /R %%i\lib\net45\PresentationTheme.*.Win10.dll`


How do I use the theme?
-----------------------

There are three different ways to use the theme. The differences are summarized
in the following table:

Feature               | Theme ResourceUri | Theme Policy               | Theme Policy with custom resource loader
----------------------|:-----------------:|:--------------------------:|:----------------------------------------:
Dynamic Theme Changes | Manually          | Yes                        | Yes
Implicit Theme Styles | No                | Yes                        | Yes
On-demand Loading     | No                | No                         | Yes


### 1. Using Theme ResourceUri

The simplest but also most limited way to use the theme is to include the
theme resources directly. [AeroTheme.ResourceUri](xref:PresentationTheme.Aero.AeroTheme.ResourceUri)
returns the Pack URI for the theme resources matching the current system theme.
To handle system theme changes subscribe to
[AeroTheme.ResourceUriChanged](xref:PresentationTheme.Aero.AeroTheme.ResourceUriChanged)
and reload the resources.

```cs
using PresentationTheme.Aero;

public partial class App
{
    public App()
    {
        // Add theme resources to the application
        Resources.MergedDictionaries.Add(new ResourceDictionary {
            Source = AeroTheme.ResourceUri
        });

        // Optional: Handle theme changes.
        AeroTheme.ResourceUriChanged += OnAeroThemeChanged;
    }

    private void OnAeroThemeChanged(object sender, EventArgs args)
    {
        // Reload theme resources
        Resources.MergedDictionaries[0].Source = AeroTheme.ResourceUri;
    }
}
```

The downside to this approach is that the resources are not treated as theme
resources by WPF and will not be used as implicit theme styles. That means:

- Whenever you define custom styles for standard controls you have to explicitly
  add a `BasedOn` attribute and reference the previous style:

  ```xaml
  <Style TargetType="{x:Type Button}" BasedOn="{StaticResource {x:Type Button}}">
  </Style>
  ```

- Even without a custom style, certain controls require an explicit style attribute.
  For example, a [ListView](xref:System.Windows.Controls.ListView) without
  [GridView](xref:System.Windows.Controls.GridView) normally inherits the
  [ListBox](xref:System.Windows.Controls.ListBox) style automatically but now
  requires an explicit style attribute.

  ```xaml
  <ListView Style="{StaticResource {x:Type ListBox}}"/>
  ```

- Some elements like the context menu of a [TextBox](xref:System.Windows.Controls.TextBox)
  use the default theme style.


### 2. Using a Theme Policy

The [ThemeManager](xref:PresentationTheme.Aero.ThemeManager) allows setting
system theme resources on a per-assembly basis. Doing so uses reflection to set
the internal resource dictionary before WPF gets a chance to load the default one.

Replacing the system theme resources solves the downsides of the previous approach
and the theme can be used as a drop-in replacement.

```cs
using PresentationTheme.Aero;

public partial class App
{
    public App()
    {
        // Set theme resources
        AeroTheme.SetAsCurrentTheme();

        // Shortcut for:
        // ThemeManager.SetPresentationFrameworkTheme(new AeroThemePolicy());
    }
}
```

These calls eventuelly invoke [SetTheme](xref:PresentationTheme.Aero.ThemeManager.SetTheme(System.Reflection.Assembly,PresentationTheme.Aero.IThemePolicy))
to set the system theme resources. This must be done before WPF attempts to load
the default resources. Otherwise you have to fake a `WM_THEMECHANGED` message to
get WPF to reapply all system styles.

Theme policies are automatically reapplied whenever the system theme changes.
Using the provided [AeroThemePolicy](xref:PresentationTheme.Aero.AeroThemePolicy)
takes care of choosing the appropriate theme resources matching the system theme.
It chooses the following theme resource assemblies:

System Theme                     | Theme Resource Assembly
---------------------------------|-----------------------------------------
Windows 10 with Aero theme       | PresentationTheme.Aero.Win10.dll        
Windows 10 with AeroLite theme   | PresentationTheme.AeroLite.Win10.dll    
Windows 10 in high contrast mode | PresentationTheme.HighContrast.Win10.dll

> [!WARNING]
> It might be tempting to use a policy that always returns for example Aero
> regardless of system theme to avoid styling and testing all themes. This will
> not work reliably since all themes use system colors, mostly for text, and an
> inverted system theme like high contrast mode can still lead to unreadable
> text.


### 3. Using a Theme Policy with custom resource loader

The third and most invasive approach replaces the internal system resource loading
mechanism via splicing (i.e., patching code at runtime). The custom resource loader
enables on-demand usage of theme policies and allows control libraries to provide
styles for specific Windows versions.

```cs
using PresentationTheme.Aero;

public partial class App
{
    public App()
    {
        if (ThemeManager.IsOperational)
            ThemeManager.Install();
        AeroTheme.SetAsCurrentTheme();
    }
}
```


What about external resource assemblies or third-party libraries?
-----------------------------------------------------------------

With the custom resource loader, theme resources for an assembly are resolved as follows:

1. If a theme policy is registered for the assembly, use the resource
   dictionary located using the pack URI returned by
   [IThemePolicy.GetCurrentThemeUri](xref:PresentationTheme.Aero.IThemePolicy.GetCurrentThemeUri).
2. If theme resources are internal, i.e. [ThemeDictionaryLocation](xref:System.Windows.ThemeInfoAttribute.ThemeDictionaryLocation)
   is [SourceAssembly](xref:System.Windows.ResourceDictionaryLocation.SourceAssembly),
   use the resource dictionary in the same assembly located at
   `pack://application:,,,/{AssemblyName};v{Version};component/themes/{ThemeName}.{ThemeColor}.xaml`.
3. Use the external resource assembly at
   `pack://application:,,,/{AssemblyName}.{ThemeName};v{Version};component/themes/{ThemeName}.{ThemeColor}.xaml`.
4. Fall back to the default theme name and use `pack://application:,,,/{AssemblyName};v{Version};component/themes/{FallbackThemeName}.{ThemeColor}.xaml` (if resources are internal)
5. Use the external resource assembly at `pack://application:,,,/{AssemblyName}.{FallbackThemeName};v{Version};component/themes/{FallbackThemeName}.{ThemeColor}.xaml`

with the following meaning for the placeholders:

- `{AssemblyName}`: [Assembly name](xref:System.Reflection.AssemblyName.Name) (`AcmeLibrary`)
- `{Version}`: [Assembly version](xref:System.Reflection.AssemblyName.Version) (`1.0.0.0`)
- `{ThemeName}`: Basename of the theme file path returned by
                [GetCurrentThemeName](https://msdn.microsoft.com/en-us/library/windows/desktop/bb773365.aspx)
                with a Windows version suffix (`Win10`, `Win8`, `Win7`).
                For the Windows 8 and 10 high contrast theme, `HighContrast` is
                used even though the theme is based on AeroLite.
- `{FallbackThemeName}`: Basename of the theme file path. `Aero` is replaced by `Aero2` if Windows 8 or later.
- `{ThemeColor}`: Theme color name returned by [GetCurrentThemeName](https://msdn.microsoft.com/en-us/library/windows/desktop/bb773365.aspx) (`NormalColor`)

Steps 4 and 5 are equivalent to the default WPF behavior.

For example, for an assembly `AcmeLibrary.dll` with version `1.0.0.0`, the
following locations are used in order:

- On Windows 10 with the Aero theme:
  1. `pack://application:,,,/AcmeLibrary;v1.0.0.0;component/themes/Aero.Win10.NormalColor.xaml` (if the resources are internal)
  2. `pack://application:,,,/AcmeLibrary.Aero.Win10;v1.0.0.0;component/themes/Aero.Win10.NormalColor.xaml`
  3. `pack://application:,,,/AcmeLibrary;v1.0.0.0;component/themes/Aero2.NormalColor.xaml` (if the resources are internal)
  4. `pack://application:,,,/AcmeLibrary.Aero2;v1.0.0.0;component/themes/Aero2.NormalColor.xaml`

- On Windows 8 with the AeroLite theme:
  1. `pack://application:,,,/AcmeLibrary;v1.0.0.0;component/themes/AeroLite.Win8.NormalColor.xaml` (if theme resources are internal)
  2. `pack://application:,,,/AcmeLibrary.AeroLite.Win8;v1.0.0.0;component/themes/AeroLite.Win8.NormalColor.xaml`
  3. `pack://application:,,,/AcmeLibrary;v1.0.0.0;component/themes/Aero2.NormalColor.xaml` (if theme resources are internal)
  4. `pack://application:,,,/AcmeLibrary.Aero2;v1.0.0.0;component/themes/Aero2.NormalColor.xaml`
