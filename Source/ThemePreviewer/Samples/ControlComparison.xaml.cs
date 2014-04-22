﻿namespace ThemePreviewer.Samples
{
    using System;
    using System.Collections.ObjectModel;
    using System.ComponentModel;
    using System.Windows;
    using System.Windows.Controls.Primitives;
    using System.Windows.Media;
    using System.Windows.Threading;

    public partial class ControlComparison
    {
        /// <summary>
        ///   Identifies the <see cref="Label"/> dependency property.
        /// </summary>
        public static readonly DependencyProperty LabelProperty =
            DependencyProperty.Register(
                nameof(Label),
                typeof(string),
                typeof(ControlComparison),
                new PropertyMetadata(null));

        /// <summary>
        ///   Identifies the <see cref="WpfSample"/> dependency property.
        /// </summary>
        public static readonly DependencyProperty WpfSampleProperty =
            DependencyProperty.Register(
                nameof(WpfSample),
                typeof(FrameworkElement),
                typeof(ControlComparison),
                new PropertyMetadata(null, OnWpfSampleChanged));

        /// <summary>
        ///   Identifies the <see cref="NativeSample"/> dependency property.
        /// </summary>
        public static readonly DependencyProperty NativeSampleProperty =
            DependencyProperty.Register(
                nameof(NativeSample),
                typeof(System.Windows.Forms.Control),
                typeof(ControlComparison),
                new PropertyMetadata(null, OnNativeSampleChanged));

        public ControlComparison(string name)
        {
            InitializeComponent();
            DataContext = this;
            Label = name;
        }

        /// <summary>
        ///   Gets or sets the Label of the <see cref="ControlComparison"/>.
        /// </summary>
        public string Label
        {
            get { return (string)GetValue(LabelProperty); }
            set { SetValue(LabelProperty, value); }
        }

        public ObservableCollection<Option> Options { get; } =
            new ObservableCollection<Option>();

        /// <summary>
        ///   Gets or sets the WPF sample of the <see cref="ControlComparison"/>.
        /// </summary>
        public FrameworkElement WpfSample
        {
            get { return (FrameworkElement)GetValue(WpfSampleProperty); }
            set { SetValue(WpfSampleProperty, value); }
        }

        /// <summary>
        ///   Gets or sets the native sample of the <see cref="ControlComparison"/>.
        /// </summary>
        public System.Windows.Forms.Control NativeSample
        {
            get { return (System.Windows.Forms.Control)GetValue(NativeSampleProperty); }
            set { SetValue(NativeSampleProperty, value); }
        }

        public static ControlComparison Create<TNative, TWpf>(string name)
            where TWpf : FrameworkElement, new()
            where TNative : System.Windows.Forms.Control, new()
        {
            return new ControlComparison(name) {
                NativeSample = new TNative(),
                WpfSample = new TWpf()
            };
        }

        private static void OnWpfSampleChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
        {
            var sample = (ControlComparison)d;
            var oldValue = (FrameworkElement)e.OldValue;
            var newValue = (FrameworkElement)e.NewValue;
            sample.OnWpfSampleChanged(oldValue, newValue);
        }

        private static void OnNativeSampleChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
        {
            var sample = (ControlComparison)d;
            var oldValue = (System.Windows.Forms.Control)e.OldValue;
            var newValue = (System.Windows.Forms.Control)e.NewValue;
            sample.OnNativeSampleChanged(oldValue, newValue);
        }

        private void OnNativeSampleChanged(
            System.Windows.Forms.Control oldValue, System.Windows.Forms.Control newValue)
        {
            WinFormsHost.Child = newValue;
            UpdateOptions(oldValue, newValue);
        }

        private void OnWpfSampleChanged(FrameworkElement oldValue, FrameworkElement newValue)
        {
            UpdateOptions(oldValue, newValue);
            App.Current.UntrackElement(oldValue);
            App.Current.TrackElement(newValue);
        }

        private void UpdateOptions(object oldValue, object newValue)
        {
            var oldOptionControl = oldValue as IOptionControl;
            if (oldOptionControl != null) {
                foreach (var option in oldOptionControl.Options) {
                    option.PropertyChanged -= OnOptionChanged;
                    Options.Remove(option);
                }
            }

            var newOptionControl = newValue as IOptionControl;
            if (newOptionControl != null) {
                foreach (var option in newOptionControl.Options) {
                    option.PropertyChanged += OnOptionChanged;
                    Options.Add(option);
                }
            }
        }

        private void OnOptionChanged(object sender, PropertyChangedEventArgs args)
        {
        }

        private void OnShowNativeClicked(object sender, RoutedEventArgs args)
        {
            Difference = ImagingUtils.TakeSnapshot(NativeSample);
        }

        private void OnShowWpfClicked(object sender, RoutedEventArgs args)
        {
            Difference = ImagingUtils.TakeSnapshot(WpfSample);
        }

        private void OnShowDiffClicked(object sender, RoutedEventArgs args)
        {
            ShowDifference();

        }

        private void OnAutoDiffChecked(object sender, RoutedEventArgs args)
        {
            var source = (ToggleButton)sender;

            timer?.Stop();
            timer = null;

            if (source.IsChecked == true) {
                timer = new DispatcherTimer(
                    TimeSpan.FromMilliseconds(500), DispatcherPriority.ContextIdle,
                    (s, e) => ShowDifference(), Dispatcher);
                timer.Start();
            }

            ShowDifference();
        }

        private DispatcherTimer timer;

        private void ShowDifference()
        {
            if (NativeSample == null || !NativeSample.IsHandleCreated || NativeSample.IsDisposed ||
                WpfSample == null || !WpfSample.IsLoaded) {
                Difference = null;
                return;
            }

            var nativeBitmap = ImagingUtils.TakeSnapshot(NativeSample);
            var wpfBitmap = ImagingUtils.TakeSnapshot(WpfSample);
            Difference = ImagingUtils.Difference(nativeBitmap, wpfBitmap);
        }

        public static readonly DependencyProperty DifferenceProperty =
            DependencyProperty.Register(
                nameof(Difference),
                typeof(ImageSource),
                typeof(ControlComparison),
                new PropertyMetadata(null));

        public ImageSource Difference
        {
            get { return (ImageSource)GetValue(DifferenceProperty); }
            set { SetValue(DifferenceProperty, value); }
        }
    }
}
