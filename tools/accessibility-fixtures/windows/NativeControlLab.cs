using System;
using System.Collections.Generic;
using System.Globalization;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Automation;
using System.Windows.Automation.Peers;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using System.Windows.Data;
using System.Windows.Documents;
using System.Windows.Forms.Integration;
using System.Windows.Input;
using System.Windows.Interop;
using System.Windows.Media;
using System.Windows.Shapes;
using System.Windows.Threading;
using Forms = System.Windows.Forms;

[assembly: System.Runtime.Versioning.TargetFramework(".NETFramework,Version=v4.8", FrameworkDisplayName = ".NET Framework 4.8")]

public sealed class NativeControlLab : Window
{
    const double PageHeight = 555;
    const string InitialNotes = "Plan a quiet morning in Gore. Visit the gallery after lunch.\nThis paragraph is ready for selection and formatting.";
    readonly TextBlock status = new TextBlock { Text = "Last action: none" };
    readonly string[] cities = { "Gore", "Queenstown", "Dunedin" };
    DataGrid reservations;
    TreeView outline;
    RichTextBox editor;
    System.Drawing.Color reportColor = System.Drawing.SystemColors.Highlight;
    int count;

    static T Named<T>(T control, string name) where T : DependencyObject
    {
        AutomationProperties.SetName(control, name);
        return control;
    }

    static T Place<T>(Canvas page, T control, double x, double y, double width, double height) where T : FrameworkElement
    {
        Canvas.SetLeft(control, x);
        Canvas.SetTop(control, PageHeight - y - height);
        control.Width = width;
        control.Height = height;
        page.Children.Add(control);
        return control;
    }

    static TextBlock Label(Canvas page, string text, double x, double y, double width, double height)
    {
        return Place(page, new TextBlock { Text = text, VerticalAlignment = VerticalAlignment.Center }, x, y, width, height);
    }

    static Button Button(Canvas page, string title, double x, double y, double width, double height, Action action)
    {
        var button = Named(new Button { Content = title }, title);
        button.Click += delegate { action(); };
        return Place(page, button, x, y, width, height);
    }

    void Say(string message)
    {
        status.Text = "Last action: " + message;
        AutomationProperties.SetName(status, status.Text);
    }

    void Changed(string name, object value)
    {
        Say(name + " = " + Convert.ToString(value, CultureInfo.InvariantCulture));
    }

    public NativeControlLab(bool background)
    {
        Title = "AppSandbox Native Control Lab";
        Width = 1000;
        Height = 790;
        MinWidth = 960;
        MinHeight = 750;
        Left = 180;
        Top = 100;
        ShowActivated = !background;
        UseLayoutRounding = true;
        var root = new DockPanel();
        Content = root;
        var menu = CreateMenu();
        DockPanel.SetDock(menu, Dock.Top);
        root.Children.Add(menu);
        var heading = new TextBlock { Text = "Native Windows controls", FontSize = 22, FontWeight = FontWeights.Bold, Margin = new Thickness(24, 14, 24, 8) };
        DockPanel.SetDock(heading, Dock.Top);
        root.Children.Add(heading);
        status.Margin = new Thickness(24, 8, 24, 14);
        status.Height = 28;
        AutomationProperties.SetAutomationId(status, "lab-status");
        DockPanel.SetDock(status, Dock.Bottom);
        root.Children.Add(status);
        var tabs = Named(new TabControl { Margin = new Thickness(20, 0, 20, 0) }, "Native fixture sections");
        root.Children.Add(tabs);
        CreateControls(AddPage(tabs, "Controls"));
        CreateLists(AddPage(tabs, "Lists and outline"));
        CreateTextEditing(AddPage(tabs, "Text editing"));
        Say("none");
        Loaded += delegate
        {
            if (background) SetWindowPos(new WindowInteropHelper(this).Handle, new IntPtr(1), 0, 0, 0, 0, 0x13);
            else Activate();
        };
    }

    static Canvas AddPage(TabControl tabs, string title)
    {
        var page = new Canvas { Width = 930, Height = PageHeight, HorizontalAlignment = HorizontalAlignment.Left, VerticalAlignment = VerticalAlignment.Top };
        tabs.Items.Add(new TabItem { Header = title, Content = page });
        return page;
    }

    Menu CreateMenu()
    {
        var menu = new Menu();
        var application = new MenuItem { Header = "Control Lab" };
        var quit = new MenuItem { Header = "Quit Control Lab", InputGestureText = "Ctrl+Q" };
        quit.Click += delegate { Close(); };
        application.Items.Add(quit);
        menu.Items.Add(application);
        CommandBindings.Add(new CommandBinding(ApplicationCommands.Close, delegate { Close(); }));
        InputBindings.Add(new KeyBinding(ApplicationCommands.Close, Key.Q, ModifierKeys.Control));
        var edit = new MenuItem { Header = "Edit" };
        AddEditCommand(edit, "Undo", ApplicationCommands.Undo);
        AddEditCommand(edit, "Cut", ApplicationCommands.Cut);
        AddEditCommand(edit, "Copy", ApplicationCommands.Copy);
        AddEditCommand(edit, "Paste", ApplicationCommands.Paste);
        AddEditCommand(edit, "Select All", ApplicationCommands.SelectAll);
        menu.Items.Add(edit);
        var format = new MenuItem { Header = "Format" };
        var font = new MenuItem { Header = "Font" };
        AddFontAction(font, "Show Fonts", ShowFonts);
        AddEditCommand(font, "Bold", EditingCommands.ToggleBold);
        AddEditCommand(font, "Italic", EditingCommands.ToggleItalic);
        AddEditCommand(font, "Underline", EditingCommands.ToggleUnderline);
        font.Items.Add(new MenuItem { Header = "Outline", IsEnabled = false });
        AddFontAction(font, "Styles…", ShowFonts);
        AddEditCommand(font, "Larger", EditingCommands.IncreaseFontSize);
        AddEditCommand(font, "Smaller", EditingCommands.DecreaseFontSize);
        var kern = new MenuItem { Header = "Kern" };
        AddFontAction(kern, "Use Default", delegate(RichTextBox target) { target.Selection.ApplyPropertyValue(Typography.KerningProperty, true); });
        AddFontAction(kern, "Use None", delegate(RichTextBox target) { target.Selection.ApplyPropertyValue(Typography.KerningProperty, false); });
        kern.Items.Add(new MenuItem { Header = "Tighten", IsEnabled = false });
        kern.Items.Add(new MenuItem { Header = "Loosen", IsEnabled = false });
        font.Items.Add(kern);
        var ligature = new MenuItem { Header = "Ligature" };
        AddFontAction(ligature, "Use Default", delegate(RichTextBox target) { SetLigatures(target, true, false); });
        AddFontAction(ligature, "Use None", delegate(RichTextBox target) { SetLigatures(target, false, false); });
        AddFontAction(ligature, "Use All", delegate(RichTextBox target) { SetLigatures(target, true, true); });
        font.Items.Add(ligature);
        var baseline = new MenuItem { Header = "Baseline" };
        AddFontAction(baseline, "Use Default", delegate(RichTextBox target) { target.Selection.ApplyPropertyValue(Inline.BaselineAlignmentProperty, BaselineAlignment.Baseline); });
        AddFontAction(baseline, "Superscript", delegate(RichTextBox target) { target.Selection.ApplyPropertyValue(Inline.BaselineAlignmentProperty, BaselineAlignment.Superscript); });
        AddFontAction(baseline, "Subscript", delegate(RichTextBox target) { target.Selection.ApplyPropertyValue(Inline.BaselineAlignmentProperty, BaselineAlignment.Subscript); });
        baseline.Items.Add(new MenuItem { Header = "Raise", IsEnabled = false });
        baseline.Items.Add(new MenuItem { Header = "Lower", IsEnabled = false });
        font.Items.Add(baseline);
        AddFontAction(font, "Show Colors", ShowTextColors);
        AddFontAction(font, "Copy Style", CopyFontStyle);
        AddFontAction(font, "Paste Style", PasteFontStyle);
        format.Items.Add(font);
        menu.Items.Add(format);
        return menu;
    }

    static void AddEditCommand(MenuItem menu, string title, ICommand command)
    {
        menu.Items.Add(new MenuItem { Header = title, Command = command });
    }

    RichTextBox FontTarget()
    {
        var target = FocusManager.GetFocusedElement(this) as RichTextBox;
        return target != null && target.IsVisible ? target : null;
    }

    void AddFontAction(MenuItem menu, string title, Action<RichTextBox> action)
    {
        var item = new MenuItem { Header = title };
        menu.SubmenuOpened += delegate { item.IsEnabled = FontTarget() != null; };
        item.Click += delegate { var target = FontTarget(); if (target != null) action(target); };
        menu.Items.Add(item);
    }

    static void SetLigatures(RichTextBox target, bool standard, bool discretionary)
    {
        target.Selection.ApplyPropertyValue(Typography.StandardLigaturesProperty, standard);
        target.Selection.ApplyPropertyValue(Typography.ContextualLigaturesProperty, standard);
        target.Selection.ApplyPropertyValue(Typography.DiscretionaryLigaturesProperty, discretionary);
        target.Selection.ApplyPropertyValue(Typography.HistoricalLigaturesProperty, discretionary);
    }

    void ShowFonts(RichTextBox target)
    {
        var family = target.Selection.GetPropertyValue(TextElement.FontFamilyProperty) as FontFamily ?? target.FontFamily;
        object sizeValue = target.Selection.GetPropertyValue(TextElement.FontSizeProperty);
        double size = sizeValue is double ? (double)sizeValue : target.FontSize;
        var style = System.Drawing.FontStyle.Regular;
        if (Equals(target.Selection.GetPropertyValue(TextElement.FontWeightProperty), FontWeights.Bold)) style |= System.Drawing.FontStyle.Bold;
        if (Equals(target.Selection.GetPropertyValue(TextElement.FontStyleProperty), FontStyles.Italic)) style |= System.Drawing.FontStyle.Italic;
        using (var initial = new System.Drawing.Font(family.Source, (float)(size * 72 / 96), style))
        using (var dialog = new Forms.FontDialog { Font = initial, ShowEffects = true })
        {
            if (dialog.ShowDialog(new DialogOwner(new WindowInteropHelper(this).Handle)) != Forms.DialogResult.OK) return;
            var chosen = dialog.Font;
            target.Selection.ApplyPropertyValue(TextElement.FontFamilyProperty, new FontFamily(chosen.FontFamily.Name));
            target.Selection.ApplyPropertyValue(TextElement.FontSizeProperty, (double)chosen.SizeInPoints * 96 / 72);
            target.Selection.ApplyPropertyValue(TextElement.FontWeightProperty, chosen.Bold ? FontWeights.Bold : FontWeights.Normal);
            target.Selection.ApplyPropertyValue(TextElement.FontStyleProperty, chosen.Italic ? FontStyles.Italic : FontStyles.Normal);
            target.Selection.ApplyPropertyValue(Inline.TextDecorationsProperty, chosen.Underline ? TextDecorations.Underline : new TextDecorationCollection());
        }
    }

    void ShowTextColors(RichTextBox target)
    {
        var brush = target.Selection.GetPropertyValue(TextElement.ForegroundProperty) as SolidColorBrush;
        var initial = brush == null ? System.Drawing.Color.Black : System.Drawing.Color.FromArgb(brush.Color.A, brush.Color.R, brush.Color.G, brush.Color.B);
        using (var dialog = new Forms.ColorDialog { Color = initial })
        {
            if (dialog.ShowDialog(new DialogOwner(new WindowInteropHelper(this).Handle)) != Forms.DialogResult.OK) return;
            var selected = dialog.Color;
            target.Selection.ApplyPropertyValue(TextElement.ForegroundProperty, new SolidColorBrush(Color.FromArgb(selected.A, selected.R, selected.G, selected.B)));
        }
    }

    static readonly DependencyProperty[] FontProperties =
    {
        TextElement.FontFamilyProperty, TextElement.FontSizeProperty, TextElement.FontWeightProperty,
        TextElement.FontStyleProperty, Inline.TextDecorationsProperty, TextElement.ForegroundProperty
    };

    static void CopyFontStyle(RichTextBox target)
    {
        var values = new string[FontProperties.Length];
        for (int i = 0; i < values.Length; ++i)
        {
            object value = target.Selection.GetPropertyValue(FontProperties[i]);
            if (value != DependencyProperty.UnsetValue && value != null)
                values[i] = TypeDescriptor.GetConverter(FontProperties[i].PropertyType).ConvertToInvariantString(value);
        }
        Clipboard.SetData("NativeControlLab.FontStyle", values);
    }

    static void PasteFontStyle(RichTextBox target)
    {
        var values = Clipboard.GetData("NativeControlLab.FontStyle") as string[];
        if (values == null || values.Length != FontProperties.Length) return;
        for (int i = 0; i < values.Length; ++i)
            if (values[i] != null) target.Selection.ApplyPropertyValue(FontProperties[i], TypeDescriptor.GetConverter(FontProperties[i].PropertyType).ConvertFromInvariantString(values[i]));
    }

    static ToggleButton NotificationSwitch()
    {
        var toggle = Named(new ToggleButton { IsChecked = false }, "Notifications");
        var track = new FrameworkElementFactory(typeof(Border));
        track.Name = "track";
        track.SetValue(Border.CornerRadiusProperty, new CornerRadius(12));
        track.SetValue(Border.BackgroundProperty, Brushes.Gray);
        track.SetValue(FrameworkElement.HeightProperty, 24.0);
        var thumb = new FrameworkElementFactory(typeof(Ellipse));
        thumb.Name = "thumb";
        thumb.SetValue(Shape.FillProperty, Brushes.White);
        thumb.SetValue(FrameworkElement.WidthProperty, 18.0);
        thumb.SetValue(FrameworkElement.HeightProperty, 18.0);
        thumb.SetValue(FrameworkElement.MarginProperty, new Thickness(3));
        thumb.SetValue(FrameworkElement.HorizontalAlignmentProperty, HorizontalAlignment.Left);
        track.AppendChild(thumb);
        var template = new ControlTemplate(typeof(ToggleButton)) { VisualTree = track };
        var on = new Trigger { Property = ToggleButton.IsCheckedProperty, Value = true };
        on.Setters.Add(new Setter(Border.BackgroundProperty, SystemColors.HighlightBrush, "track"));
        on.Setters.Add(new Setter(FrameworkElement.HorizontalAlignmentProperty, HorizontalAlignment.Right, "thumb"));
        template.Triggers.Add(on);
        toggle.Template = template;
        return toggle;
    }

    static RadioButton Segment(string name)
    {
        var segment = Named(new RadioButton { Content = name, GroupName = "Time of day", IsChecked = name == "Morning" }, name);
        var border = new FrameworkElementFactory(typeof(Border));
        border.Name = "border";
        border.SetValue(Border.BackgroundProperty, SystemColors.ControlBrush);
        border.SetValue(Border.BorderBrushProperty, SystemColors.ActiveBorderBrush);
        border.SetValue(Border.BorderThicknessProperty, new Thickness(1));
        var content = new FrameworkElementFactory(typeof(ContentPresenter));
        content.SetBinding(ContentPresenter.ContentProperty, new Binding("Content") { RelativeSource = new RelativeSource(RelativeSourceMode.TemplatedParent) });
        content.SetValue(FrameworkElement.HorizontalAlignmentProperty, HorizontalAlignment.Center);
        content.SetValue(FrameworkElement.VerticalAlignmentProperty, VerticalAlignment.Center);
        border.AppendChild(content);
        var template = new ControlTemplate(typeof(RadioButton)) { VisualTree = border };
        var selected = new Trigger { Property = ToggleButton.IsCheckedProperty, Value = true };
        selected.Setters.Add(new Setter(Border.BackgroundProperty, SystemColors.HighlightBrush, "border"));
        selected.Setters.Add(new Setter(Control.ForegroundProperty, SystemColors.HighlightTextBrush));
        template.Triggers.Add(selected);
        segment.Template = template;
        return segment;
    }

    void CreateControls(Canvas page)
    {
        Button(page, "Count press", 24, 490, 150, 34, delegate { Say("Count press " + (++count)); });
        Button(page, "Unavailable", 190, 490, 150, 34, delegate { Changed("Unavailable", 0); }).IsEnabled = false;
        var meal = Named(new CheckBox { Content = "Meal included", IsChecked = false }, "Meal included");
        meal.Checked += delegate { Changed("Meal included", 1); };
        meal.Unchecked += delegate { Changed("Meal included", 0); };
        Place(page, meal, 24, 436, 190, 30);
        var notifications = NotificationSwitch();
        notifications.Checked += delegate { Changed("Notifications", 1); };
        notifications.Unchecked += delegate { Changed("Notifications", 0); };
        Place(page, notifications, 245, 436, 54, 30);
        Label(page, "Notifications", 310, 440, 150, 24);
        foreach (string seat in new[] { "Window seat", "Aisle seat" })
        {
            string name = seat;
            var radio = Named(new RadioButton { Content = name, GroupName = "Seat", IsChecked = name == "Window seat" }, name);
            radio.Checked += delegate { Changed(name, 1); };
            Place(page, radio, name == "Window seat" ? 490 : 660, 438, 150, 26);
        }
        Label(page, "Volume", 24, 380, 100, 24);
        var slider = Named(new Slider { Minimum = 0, Maximum = 10, Value = 4 }, "Volume");
        var volumeValue = Label(page, "4.0", 385, 380, 44, 24);
        slider.ValueChanged += delegate { volumeValue.Text = slider.Value.ToString("F1", CultureInfo.InvariantCulture); Changed("Volume", slider.Value); };
        Place(page, slider, 140, 375, 240, 30);
        Label(page, "Travelers", 430, 380, 100, 24);
        var stepper = new Forms.NumericUpDown { Minimum = 1, Maximum = 9, Value = 2, Increment = 1, DecimalPlaces = 0, AccessibleName = "Travelers", Dock = Forms.DockStyle.Fill };
        var travelerValue = Label(page, "2", 630, 380, 40, 24);
        stepper.ValueChanged += delegate { travelerValue.Text = stepper.Value.ToString(CultureInfo.InvariantCulture); Changed("Travelers", stepper.Value); };
        Place(page, new WindowsFormsHost { Child = stepper }, 550, 372, 65, 32);
        var destination = Named(new ComboBox(), "Destination");
        foreach (string city in cities) destination.Items.Add(city);
        destination.SelectedIndex = 0;
        destination.SelectionChanged += delegate { Changed("Destination", destination.SelectedItem); };
        Place(page, destination, 24, 308, 230, 34);
        var activity = Named(new ComboBox { IsEditable = true }, "Activity");
        foreach (string item in new[] { "Art gallery", "Fishing", "Aviation museum" }) activity.Items.Add(item);
        activity.Text = "Art gallery";
        activity.SelectionChanged += delegate { Dispatcher.BeginInvoke(new Action(delegate { Changed("Activity", activity.Text); })); };
        activity.LostKeyboardFocus += delegate { Changed("Activity", activity.Text); };
        activity.KeyDown += delegate(object sender, KeyEventArgs e) { if (e.Key == Key.Return) Changed("Activity", activity.Text); };
        Place(page, activity, 305, 310, 240, 30);
        var periods = Named(new UniformGrid { Rows = 1, Columns = 3 }, "Time of day");
        foreach (string period in new[] { "Morning", "Afternoon", "Evening" })
        {
            string name = period;
            var segment = Segment(name);
            segment.Checked += delegate { Changed("Time of day", name); };
            periods.Children.Add(segment);
        }
        Place(page, periods, 24, 250, 370, 32);
        var date = Named(new DatePicker { SelectedDate = new DateTime(1970, 1, 1, 0, 0, 0, DateTimeKind.Utc).AddSeconds(1790208000).ToLocalTime().Date }, "Departure date");
        date.SelectedDateChanged += delegate { Changed("Departure date", date.SelectedDate); };
        Place(page, date, 24, 190, 230, 30);
        var color = Named(new Button { Background = SystemColors.HighlightBrush, ToolTip = "Report color" }, "Report color");
        color.Click += delegate
        {
            using (var dialog = new Forms.ColorDialog { Color = reportColor })
            {
                if (dialog.ShowDialog(new DialogOwner(new WindowInteropHelper(this).Handle)) == Forms.DialogResult.OK)
                {
                    var chosen = dialog.Color;
                    reportColor = chosen;
                    color.Background = new SolidColorBrush(Color.FromArgb(chosen.A, chosen.R, chosen.G, chosen.B));
                    Changed("Report color", chosen);
                }
            }
        };
        Place(page, color, 310, 185, 70, 34);
        Place(page, Named(new ProgressBar { Minimum = 0, Maximum = 1, Value = .65 }, "Download progress"), 24, 128, 300, 20);
        Place(page, Named(new ProgressBar { Minimum = 0, Maximum = 100, Value = 40 }, "Disk usage"), 375, 128, 220, 22);
        Button(page, "Open sheet", 24, 60, 180, 34, OpenSheet);
        Button(page, "Delayed update", 220, 60, 180, 34, DelayedUpdate);
    }

    void OpenSheet()
    {
        var dialog = new Window { Title = "Trip confirmation", Owner = this, Width = 410, Height = 190, WindowStartupLocation = WindowStartupLocation.CenterOwner, ResizeMode = ResizeMode.NoResize, ShowInTaskbar = false };
        var panel = new StackPanel { Margin = new Thickness(20) };
        panel.Children.Add(new TextBlock { Text = "Trip confirmation", FontWeight = FontWeights.Bold, FontSize = 18 });
        panel.Children.Add(new TextBlock { Text = "A native sheet for accessibility testing.", Margin = new Thickness(0, 12, 0, 18) });
        var buttons = new StackPanel { Orientation = Orientation.Horizontal, HorizontalAlignment = HorizontalAlignment.Right };
        var confirm = Named(new Button { Content = "Confirm trip", MinWidth = 110, Padding = new Thickness(8, 4, 8, 4), Margin = new Thickness(0, 0, 8, 0), IsDefault = true }, "Confirm trip");
        confirm.Click += delegate { dialog.DialogResult = true; };
        buttons.Children.Add(confirm);
        buttons.Children.Add(Named(new Button { Content = "Cancel", MinWidth = 90, Padding = new Thickness(8, 4, 8, 4), IsCancel = true }, "Cancel"));
        panel.Children.Add(buttons);
        dialog.Content = panel;
        Say(dialog.ShowDialog() == true ? "Trip confirmed" : "Trip cancelled");
    }

    void DelayedUpdate()
    {
        Say("Waiting for delayed update");
        var timer = new DispatcherTimer { Interval = TimeSpan.FromSeconds(3) };
        timer.Tick += delegate
        {
            timer.Stop();
            Say("Delayed update completed");
            var peer = UIElementAutomationPeer.FromElement(status) ?? UIElementAutomationPeer.CreatePeerForElement(status);
            if (peer != null) peer.RaiseAutomationEvent(AutomationEvents.LiveRegionChanged);
        };
        timer.Start();
    }

    void CreateLists(Canvas page)
    {
        Label(page, "Select Reservation 072 without manual scrolling", 20, 513, 870, 24);
        var rows = new List<Reservation>();
        for (int row = 0; row < 120; ++row)
            rows.Add(new Reservation { Name = "Reservation " + (row + 1).ToString("000"), City = cities[row % 3] });
        reservations = Named(new DataGrid
        {
            ItemsSource = rows, AutoGenerateColumns = false, IsReadOnly = true, CanUserAddRows = false,
            SelectionMode = DataGridSelectionMode.Extended, SelectionUnit = DataGridSelectionUnit.FullRow,
            RowHeight = 27, HeadersVisibility = DataGridHeadersVisibility.Column, ColumnHeaderHeight = 24,
            VerticalScrollBarVisibility = ScrollBarVisibility.Auto, HorizontalScrollBarVisibility = ScrollBarVisibility.Auto
        }, "Reservations");
        reservations.Columns.Add(new DataGridTextColumn { Header = "Reservation", Binding = new Binding("Name"), Width = 280 });
        reservations.Columns.Add(new DataGridTextColumn { Header = "City", Binding = new Binding("City"), Width = 140 });
        ExposeCompleteItems(reservations);
        reservations.SelectionChanged += delegate
        {
            int first = Int32.MaxValue;
            foreach (Reservation selected in reservations.SelectedItems) first = Math.Min(first, rows.IndexOf(selected));
            Say(first == Int32.MaxValue ? "No reservation selected" : "Reservation " + (first + 1).ToString("000") + " selected (" + reservations.SelectedItems.Count + " total)");
        };
        Place(page, reservations, 20, 55, 520, 444);
        var outlinePanel = new DockPanel();
        var header = new TextBlock { Text = "Itinerary", Height = 24, Background = SystemColors.ControlBrush, Padding = new Thickness(4) };
        DockPanel.SetDock(header, Dock.Top);
        outlinePanel.Children.Add(header);
        outline = Named(new TreeView(), "Trip outline");
        ExposeCompleteItems(outline);
        for (int day = 1; day <= 30; ++day)
        {
            string name = "Day " + day.ToString("00");
            var group = OutlineItem(name);
            for (int activity = 1; activity <= 3; ++activity) group.Items.Add(OutlineItem(name + " activity " + activity));
            outline.Items.Add(group);
        }
        outline.SelectedItemChanged += delegate
        {
            var item = outline.SelectedItem as TreeViewItem;
            Say(item == null ? "No itinerary item selected" : item.Tag + " selected");
        };
        outlinePanel.Children.Add(outline);
        Place(page, outlinePanel, 565, 55, 335, 444);
    }

    static TreeViewItem OutlineItem(string name)
    {
        return Named(new TreeViewItem { Header = new TextBlock { Text = name, Height = 27 }, Tag = name }, name);
    }

    static void ExposeCompleteItems(DependencyObject control)
    {
        VirtualizingPanel.SetIsVirtualizing(control, false);
        ScrollViewer.SetCanContentScroll(control, false);
    }

    void CreateTextEditing(Canvas page)
    {
        Label(page, "Report title", 24, 505, 120, 24);
        var title = Named(new TextBox { Text = "Draft trip report" }, "Report title");
        title.LostKeyboardFocus += delegate { Changed("Report title", title.Text); };
        title.KeyDown += delegate(object sender, KeyEventArgs e) { if (e.Key == Key.Return) Changed("Report title", title.Text); };
        Place(page, title, 150, 500, 500, 30);
        Label(page, "Access code", 24, 455, 120, 24);
        Place(page, Named(new PasswordBox { Password = "sample-code" }, "Access code"), 150, 450, 280, 30);
        editor = Named(new RichTextBox { AcceptsReturn = true, VerticalScrollBarVisibility = ScrollBarVisibility.Auto, FontSize = 18, Document = new FlowDocument(new Paragraph(new Run(InitialNotes))) }, "Trip notes");
        SpellCheck.SetIsEnabled(editor, false);
        editor.TextChanged += delegate { Say("Trip notes edited"); };
        Place(page, editor, 24, 85, 875, 340);
        var bold = Button(page, "Bold selection", 24, 28, 180, 34, delegate { editor.Selection.ApplyPropertyValue(TextElement.FontWeightProperty, FontWeights.Bold); });
        bold.Focusable = false;
    }

    sealed class DialogOwner : Forms.IWin32Window
    {
        readonly IntPtr handle;
        public DialogOwner(IntPtr handle) { this.handle = handle; }
        public IntPtr Handle { get { return handle; } }
    }

    [DllImport("user32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    static extern bool SetWindowPos(IntPtr window, IntPtr insertAfter, int x, int y, int width, int height, uint flags);

    public sealed class Reservation
    {
        public string Name { get; set; }
        public string City { get; set; }
        public override string ToString() { return Name; }
    }

    [STAThread]
    public static void Main(string[] args)
    {
        new Application().Run(new NativeControlLab(Array.IndexOf(args, "--background") >= 0));
    }
}
