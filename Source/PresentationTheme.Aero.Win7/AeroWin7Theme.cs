namespace PresentationTheme.Aero.Win7
{
    using System;
    using System.Reflection;
    using System.Text;
    using System.Windows;

    public static class AeroWin7Theme
    {
        public static Uri ResourceUri =>
            MakePackUri(typeof(AeroWin7Theme).Assembly, "Themes/AeroWin7.NormalColor.xaml");

        public static ResourceDictionary CreateResourceDictionary()
        {
            return new ResourceDictionary { Source = ResourceUri };
        }

        private static Uri MakePackUri(Assembly assembly, string path)
        {
            var name = FormatName(assembly.GetName());
            return new Uri(
                $"pack://application:,,,/{name};component/{path}",
                UriKind.Absolute);
        }

        private static string FormatName(AssemblyName name)
        {
            return $"{name.Name};v{name.Version}{GetPublicKeySegment(name)}";
        }

        private static string GetPublicKeySegment(AssemblyName name)
        {
            var bytes = name.GetPublicKeyToken();
            if (bytes.Length == 0)
                return string.Empty;

            var builder = new StringBuilder(1 + bytes.Length * 2);
            builder.Append(';');
            foreach (var b in bytes)
                builder.AppendFormat("{0:x2}", b);

            return builder.ToString();
        }
    }
}
