// DiscGlow setup: installs the DiscGlow DLL into Echo VR (PC) and edits the disc colours it uses.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Windows.Forms;
using Microsoft.Win32;

namespace DiscGlowSetup
{
	static class Program
	{
		[STAThread]
		static void Main(string[] args)
		{
			Application.EnableVisualStyles();
			Application.SetCompatibleTextRenderingDefault(false);
			Application.Run(new MainForm(args.Length > 0 ? args[0] : null));
		}
	}

	// A glowing sphere preview of a linear RGB colour (values above 1 glow brighter in game)
	class ColourSwatch : ThemedControl
	{
		float[] _rgb = { 1, 1, 1 };
		public void SetColour(float[] rgb) { _rgb = (float[])rgb.Clone(); Invalidate(); }

		static int ToSrgb(float linear)
		{
			linear = Math.Max(0f, Math.Min(1f, linear));
			float s = linear <= 0.0031308f ? linear * 12.92f : 1.055f * (float)Math.Pow(linear, 1 / 2.4) - 0.055f;
			return (int)Math.Round(s * 255);
		}
		public static Color Display(float[] rgb)
		{
			float peak = Math.Max(1f, Math.Max(rgb[0], Math.Max(rgb[1], rgb[2])));
			return Color.FromArgb(ToSrgb(rgb[0] / peak), ToSrgb(rgb[1] / peak), ToSrgb(rgb[2] / peak));
		}

		protected override void OnPaint(PaintEventArgs e)
		{
			var g = e.Graphics;
			g.SmoothingMode = SmoothingMode.AntiAlias;
			g.Clear(Theme.Card);
			Color c = Display(_rgb);
			float peak = Math.Max(_rgb[0], Math.Max(_rgb[1], _rgb[2]));
			var r = new RectangleF(4, 4, Width - 8, Height - 8);

			// Outer glow grows with brightness
			int glow = (int)Math.Min(120, 40 + 60 * peak);
			using (var path = new GraphicsPath())
			{
				path.AddEllipse(0, 0, Width - 1, Height - 1);
				using (var brush = new PathGradientBrush(path) { CenterColor = Color.FromArgb(glow, c), SurroundColors = new[] { Color.FromArgb(0, c) } })
					g.FillEllipse(brush, 0, 0, Width - 1, Height - 1);
			}
			// Translucent sphere with a bright rim, like the disc's grid bubble
			using (var fill = new SolidBrush(Color.FromArgb(70, c)))
				g.FillEllipse(fill, r);
			using (var rim = new Pen(Color.FromArgb(230, c), 2.5f))
				g.DrawEllipse(rim, r);
			using (var grid = new Pen(Color.FromArgb(110, c), 1f))
			{
				g.DrawEllipse(grid, r.X + r.Width * 0.3f, r.Y, r.Width * 0.4f, r.Height);
				g.DrawLine(grid, r.X, r.Y + r.Height / 2, r.Right, r.Y + r.Height / 2);
			}
		}
	}

	// One colour the user can choose: RGB sliders, a preview and a reset button
	class ColourPicker
	{
		public string Key, Title, Subtitle;
		public float[] Default;
		public readonly Slider[] Sliders = new Slider[3];
		public Slider Saturation, Speed;
		public readonly Segmented Effect = new Segmented { Options = new[] { "None", "Rainbow", "Strobe", "Pulse" } };
		public static readonly string[] EffectNames = { "none", "rainbow", "strobe", "pulse" };
		public string EffectKey { get { return Key.Replace("Colour", "Effect"); } }
		public string SpeedKey { get { return Key.Replace("Colour", "EffectSpeed"); } }

		// The same effects as DiscGlow, for the preview: rainbow hue wheel, strobe on/off, pulse brightness
		public float[] Animated(double t)
		{
			float[] b = Effective;
			double period = Math.Max(0.05, (double)Speed.Value);
			double phase = (t / period) % 1.0;
			float peak = Math.Max(b[0], Math.Max(b[1], b[2]));
			switch (Effect.Selected)
			{
				case 1:
				{
					float h = (float)phase * 6f, x = 1f - Math.Abs(h % 2f - 1f), v = Math.Max(0.5f, peak);
					float[][] wheel = { new[] { 1f, x, 0f }, new[] { x, 1f, 0f }, new[] { 0f, 1f, x }, new[] { 0f, x, 1f }, new[] { x, 0f, 1f }, new[] { 1f, 0f, x } };
					return wheel[Math.Min(5, (int)h)].Select(c => c * v).ToArray();
				}
				case 2: return phase < 0.5 ? b : b.Select(c => c * 0.03f).ToArray();
				case 3:
				{
					float k = 0.3f + 0.7f * (0.5f + 0.5f * (float)Math.Cos(phase * 2 * Math.PI));
					return b.Select(c => c * k).ToArray();
				}
				default: return b;
			}
		}
		public readonly ColourSwatch Swatch = new ColourSwatch();
		public ToggleSwitch Enable; // Only for colours that can be switched off

		public string SaturationKey { get { return Key.Replace("Colour", "Saturation"); } }
		public float[] Value { get { return Sliders.Select(s => (float)s.Value).ToArray(); } }

		// The colour as it appears in game: DiscGlow scales the distance from grey of the same brightness (Rec. 709 weights)
		public float[] Effective
		{
			get
			{
				float[] rgb = Value;
				float saturation = (float)Saturation.Value;
				float luminance = 0.2126f * rgb[0] + 0.7152f * rgb[1] + 0.0722f * rgb[2];
				return rgb.Select(c => Math.Max(0f, luminance + (c - luminance) * saturation)).ToArray();
			}
		}
		public void SetValue(float[] rgb, float saturation)
		{
			for (int i = 0; i < 3; i++) Sliders[i].Value = (decimal)Math.Round(rgb[i], 2);
			Saturation.Value = (decimal)Math.Round(saturation, 2);
			Swatch.SetColour(Effective);
		}
	}

	class MainForm : Form
	{
		const string DllName = "dinput8.dll";
		const string ChainName = "dinput8.chain.dll";
		const string IniName = "DiscGlow.ini";
		const string EmbeddedDll = "DiscGlow.dll";
		const string Marker = "DiscGlow active:"; // Text inside the DiscGlow DLL, to recognise it

		// The engine function DiscGlow hooks, and the bytes it starts with in the supported build
		const uint HookRva = 0xcc5900;
		static readonly byte[] HookBytes = { 0x40, 0x53, 0x55, 0x56, 0x41, 0x54, 0x41, 0x56, 0x48, 0x83, 0xec, 0x30 };

		static readonly float[] GameBlue = { 0f, 0.698f, 1f }, GameOrange = { 1f, 0.5f, 0.15f };

		readonly ColourPicker[] _pickers =
		{
			new ColourPicker { Key = "PersonalDiscColour", Title = "Personal disc", Subtitle = "The disc you spawn", Default = new[] { 1f, 0.5f, 0.15f } },
			new ColourPicker { Key = "BlueTeamColour", Title = "Blue team", Subtitle = "Held by blue", Default = GameBlue },
			new ColourPicker { Key = "OrangeTeamColour", Title = "Orange team", Subtitle = "Held by orange", Default = GameOrange },
		};

		string _folder;
		bool _loading;
		readonly Timer _saveTimer = new Timer { Interval = 250 };
		readonly Label lblFolder = new Label();
		readonly StatusLine status = new StatusLine();
		readonly FlatButton btnInstall = new FlatButton(), btnUninstall = new FlatButton(), btnLaunch = new FlatButton();
		readonly Card colourCard = new Card();
		readonly ToggleSwitch chkSticky = new ToggleSwitch();

		static string SettingsPath { get { return Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "DiscGlow", "folder.txt"); } }
		string P(string name) { return Path.Combine(_folder, name); }

		[DllImport("dwmapi.dll")]
		static extern int DwmSetWindowAttribute(IntPtr hwnd, int attribute, ref int value, int size);
		protected override void OnHandleCreated(EventArgs e)
		{
			base.OnHandleCreated(e);
			int dark = 1;
			DwmSetWindowAttribute(Handle, 20, ref dark, 4); // DWMWA_USE_IMMERSIVE_DARK_MODE
			int caption = Theme.Back.R | (Theme.Back.G << 8) | (Theme.Back.B << 16);
			DwmSetWindowAttribute(Handle, 35, ref caption, 4); // DWMWA_CAPTION_COLOR (Windows 11)
		}

		public MainForm(string folderArgument)
		{
			Text = "DiscGlow";
			try { Icon = Icon.ExtractAssociatedIcon(Application.ExecutablePath); } catch { }
			FormBorderStyle = FormBorderStyle.FixedSingle;
			MaximizeBox = false;
			StartPosition = FormStartPosition.CenterScreen;
			ClientSize = new Size(720, 856);
			BackColor = Theme.Back;
			ForeColor = Theme.Text;
			Font = Theme.Body;
			DoubleBuffered = true;

			var lblTitle = new Label { Text = "DiscGlow", Font = Theme.Title, ForeColor = Theme.Text, AutoSize = true, Left = 22, Top = 16 };
			var lblSub = new Label { Text = "Disc sphere colours for Echo VR on PC", Font = Theme.Body, ForeColor = Theme.Muted, AutoSize = true, Left = 25, Top = 52 };

			// Install card
			var installCard = new Card { Title = "Install", Left = 20, Top = 84, Width = 680, Height = 186 };
			lblFolder.SetBounds(18, 50, 540, 20);
			lblFolder.ForeColor = Theme.Muted; lblFolder.BackColor = Theme.Card; lblFolder.AutoEllipsis = true;
			var btnBrowse = new FlatButton { Text = "Browse" };
			btnBrowse.SetBounds(572, 44, 90, 32);
			btnBrowse.Click += delegate { Browse(); };
			status.SetBounds(18, 84, 644, 44);
			btnInstall.Primary = true; btnInstall.Text = "Install / Update"; btnInstall.SetBounds(18, 136, 150, 34); btnInstall.Click += delegate { Install(); };
			btnUninstall.Text = "Uninstall"; btnUninstall.SetBounds(178, 136, 110, 34); btnUninstall.Click += delegate { Uninstall(); };
			btnLaunch.Text = "Launch Echo"; btnLaunch.SetBounds(298, 136, 130, 34); btnLaunch.Click += delegate { Launch(); };
			installCard.Controls.AddRange(new Control[] { lblFolder, btnBrowse, status, btnInstall, btnUninstall, btnLaunch });

			// Colour card
			colourCard.Title = "Disc colours";
			colourCard.SetBounds(20, 286, 680, 550);
			colourCard.Controls.Add(new Label { Text = "Changes apply straight away, even while Echo is running. Values above 1 glow brighter.",
				ForeColor = Theme.Muted, BackColor = Theme.Card, AutoSize = true, Left = 18, Top = 42 });

			const int columnWidth = 200, gap = 22, top = 74;
			string[] channel = { "Red", "Green", "Blue" };
			Color[] fills = { Color.FromArgb(235, 87, 87), Color.FromArgb(80, 200, 120), Color.FromArgb(86, 156, 255) };
			for (int i = 0; i < _pickers.Length; i++)
			{
				var picker = _pickers[i];
				int x = 18 + i * (columnWidth + gap);
				picker.Swatch.SetBounds(x, top, 56, 56);
				colourCard.Controls.Add(picker.Swatch);
				colourCard.Controls.Add(new Label { Text = picker.Title, Font = Theme.Semibold, ForeColor = Theme.Text, BackColor = Theme.Card, AutoSize = true, Left = x + 66, Top = top + 8 });
				colourCard.Controls.Add(new Label { Text = picker.Subtitle, Font = Theme.Small, ForeColor = Theme.Muted, BackColor = Theme.Card, AutoSize = true, Left = x + 66, Top = top + 30 });
				for (int c = 0; c < 4; c++)
				{
					var slider = new Slider { Text = c < 3 ? channel[c] : "Saturation", Minimum = 0, Maximum = 2, Step = 0.01m, Decimals = 2, FillColor = c < 3 ? fills[c] : Theme.Accent };
					slider.SetBounds(x, top + 72 + c * 46, columnWidth, 40);
					slider.ValueChanged += delegate { if (picker.Saturation != null && picker.Effect.Selected == 0) picker.Swatch.SetColour(picker.Effective); ColoursChanged(); };
					if (c < 3) picker.Sliders[c] = slider; else picker.Saturation = slider;
					colourCard.Controls.Add(slider);
				}
				colourCard.Controls.Add(new Label { Text = "Effect", Font = Theme.Body, ForeColor = Theme.Muted, BackColor = Theme.Card, AutoSize = true, Left = x + 5, Top = top + 256 });
				picker.Effect.SetBounds(x, top + 278, columnWidth, 28);
				picker.Effect.SelectedChanged += delegate { picker.Speed.Enabled = picker.Effect.Selected != 0 && picker.Effect.Enabled; if (picker.Effect.Selected == 0) picker.Swatch.SetColour(picker.Effective); ColoursChanged(); };
				colourCard.Controls.Add(picker.Effect);
				picker.Speed = new Slider { Text = "Seconds per cycle", Minimum = 0.1m, Maximum = 10, Step = 0.1m, Decimals = 1, Value = 2, Enabled = false };
				picker.Speed.SetBounds(x, top + 314, columnWidth, 40);
				picker.Speed.ValueChanged += delegate { ColoursChanged(); };
				colourCard.Controls.Add(picker.Speed);
				var reset = new FlatButton { Text = i == 0 ? "Reset (2018 orange)" : "Reset (game colour)" };
				reset.SetBounds(x, top + 364, columnWidth, 30);
				reset.Click += delegate { picker.SetValue(picker.Default, 1f); picker.Effect.Selected = 0; picker.Speed.Value = 2; };
				colourCard.Controls.Add(reset);
			}

			var personal = _pickers[0];
			personal.Enable = new ToggleSwitch { Text = "Colour personal discs", Description = "Off leaves them the game's plain white" };
			personal.Enable.SetBounds(18, top + 410, 320, 44);
			personal.Enable.CheckedChanged += delegate { foreach (var s in personal.Sliders) s.Enabled = personal.Enable.Checked; personal.Saturation.Enabled = personal.Enable.Checked; personal.Effect.Enabled = personal.Enable.Checked; personal.Speed.Enabled = personal.Enable.Checked && personal.Effect.Selected != 0; ColoursChanged(); };
			chkSticky.Text = "Keep the team colour";
			chkSticky.Description = "Until the other team touches it";
			chkSticky.SetBounds(350, top + 410, 312, 44);
			chkSticky.CheckedChanged += delegate { ColoursChanged(); };
			colourCard.Controls.AddRange(new Control[] { personal.Enable, chkSticky });

			Controls.AddRange(new Control[] { lblTitle, lblSub, installCard, colourCard });

			_saveTimer.Tick += delegate { _saveTimer.Stop(); SaveColours(); };
			var preview = new Timer { Interval = 33 };
			var clock = Stopwatch.StartNew();
			preview.Tick += delegate
			{
				foreach (var picker in _pickers)
					if (picker.Effect.Selected != 0 && picker.Saturation != null)
						picker.Swatch.SetColour(picker.Animated(clock.Elapsed.TotalSeconds));
			};
			preview.Start();
			Activated += delegate { if (!_saveTimer.Enabled) RefreshAll(); };
			FormClosing += delegate { if (_saveTimer.Enabled) { _saveTimer.Stop(); SaveColours(); } };

			SetFolder(folderArgument ?? LoadSetting() ?? DetectFolder());
		}

		// ---- Finding Echo ----

		static string NormalizeFolder(string path)
		{
			if (string.IsNullOrEmpty(path)) return null;
			foreach (string candidate in new[] { path, Path.Combine(path, "bin", "win10"), Path.Combine(path, "Software", "ready-at-dawn-echo-arena", "bin", "win10") })
				if (File.Exists(Path.Combine(candidate, "echovr.exe"))) return candidate;
			return null;
		}

		// Echo from the Oculus / Meta library folders listed in the registry, or the default install location
		static string DetectFolder()
		{
			var libraries = new List<string>();
			try
			{
				using (var key = Registry.CurrentUser.OpenSubKey(@"Software\Oculus VR, LLC\Oculus\Libraries"))
					if (key != null)
						foreach (string name in key.GetSubKeyNames())
							using (var library = key.OpenSubKey(name))
							{
								object path = library != null ? library.GetValue("OriginalPath") : null;
								if (path != null) libraries.Add(path.ToString());
							}
			}
			catch { }
			libraries.Add(@"C:\Program Files\Oculus\Software");
			foreach (string library in libraries)
			{
				string found = NormalizeFolder(Path.Combine(library, "Software", "ready-at-dawn-echo-arena"));
				if (found != null) return found;
			}
			return null;
		}

		static string LoadSetting()
		{
			try { return File.Exists(SettingsPath) ? NormalizeFolder(File.ReadAllText(SettingsPath).Trim()) : null; } catch { return null; }
		}

		void Browse()
		{
			using (var dlg = new FolderBrowserDialog { Description = "Select the Echo VR folder (ready-at-dawn-echo-arena, or its bin\\win10 folder)" })
			{
				if (dlg.ShowDialog(this) != DialogResult.OK) return;
				if (NormalizeFolder(dlg.SelectedPath) == null) { Msg("echovr.exe was not found in that folder or in its bin\\win10 subfolder."); return; }
				SetFolder(dlg.SelectedPath);
				try { Directory.CreateDirectory(Path.GetDirectoryName(SettingsPath)); File.WriteAllText(SettingsPath, _folder); } catch { }
			}
		}

		void SetFolder(string path)
		{
			_folder = NormalizeFolder(path);
			lblFolder.Text = _folder ?? "Echo VR was not found. Click Browse to pick its folder.";
			RefreshAll();
		}

		// ---- Checks ----

		bool IsGameRunning()
		{
			foreach (Process p in Process.GetProcessesByName("echovr"))
			{
				try { if (string.Equals(Path.GetDirectoryName(p.MainModule.FileName), _folder, StringComparison.OrdinalIgnoreCase)) return true; }
				catch { return true; } // Cannot tell which install it is, so assume this one
			}
			return false;
		}

		static bool IsDiscGlow(string path)
		{
			try { return File.Exists(path) && IndexOf(File.ReadAllBytes(path), Encoding.ASCII.GetBytes(Marker)) >= 0; } catch { return false; }
		}

		static int IndexOf(byte[] data, byte[] pattern)
		{
			for (int i = 0; i + pattern.Length <= data.Length; i++)
			{
				int k = 0;
				while (k < pattern.Length && data[i + k] == pattern[k]) k++;
				if (k == pattern.Length) return i;
			}
			return -1;
		}

		// True when echovr.exe is the build DiscGlow was made for (the hooked function starts with the expected bytes)
		static bool? IsSupportedGame(string exe)
		{
			try
			{
				byte[] d = File.ReadAllBytes(exe);
				int pe = BitConverter.ToInt32(d, 0x3c);
				int sections = BitConverter.ToUInt16(d, pe + 6), optionalSize = BitConverter.ToUInt16(d, pe + 20);
				for (int i = 0; i < sections; i++)
				{
					int s = pe + 24 + optionalSize + i * 40;
					uint va = BitConverter.ToUInt32(d, s + 12), rawSize = BitConverter.ToUInt32(d, s + 16), raw = BitConverter.ToUInt32(d, s + 20);
					if (HookRva >= va && HookRva < va + rawSize)
					{
						long offset = raw + (HookRva - va);
						return HookBytes.Select((b, k) => d[offset + k] == b).All(x => x);
					}
				}
				return false;
			}
			catch { return null; }
		}

		static byte[] _embedded;
		static byte[] EmbeddedDllBytes()
		{
			if (_embedded == null)
				using (Stream s = Assembly.GetExecutingAssembly().GetManifestResourceStream(EmbeddedDll))
				using (var m = new MemoryStream()) { s.CopyTo(m); _embedded = m.ToArray(); }
			return _embedded;
		}
		static bool SameAsEmbedded(string path)
		{
			using (var sha = SHA256.Create())
				return sha.ComputeHash(File.ReadAllBytes(path)).SequenceEqual(sha.ComputeHash(EmbeddedDllBytes()));
		}

		void RefreshAll()
		{
			bool valid = _folder != null;
			btnInstall.Enabled = btnLaunch.Enabled = valid;
			btnUninstall.Enabled = false;
			if (!valid) { status.Set("Pick your Echo VR folder to continue.", Theme.Muted); colourCard.Enabled = false; return; }

			string dll = P(DllName);
			bool installed = IsDiscGlow(dll);
			btnUninstall.Enabled = installed;
			colourCard.Enabled = installed;

			bool? supported = IsSupportedGame(P("echovr.exe"));
			var lines = new List<string>();
			Color dot = Theme.Muted;
			if (installed && SameAsEmbedded(dll)) { lines.Add("Installed."); dot = Theme.Good; }
			else if (installed) { lines.Add("Installed, but an older version. Click Install / Update."); dot = Theme.Warn; }
			else if (File.Exists(dll)) lines.Add("Not installed. Another mod uses dinput8.dll (e.g. ReShade); it will keep working alongside DiscGlow.");
			else lines.Add("Not installed.");
			if (installed && File.Exists(P(ChainName))) lines.Add("Also loading your other dinput8 mod.");
			if (supported == false) { lines.Add("This echovr.exe is a different build than DiscGlow supports, so DiscGlow will stay inactive."); dot = Theme.Warn; }
			if (IsGameRunning()) lines.Add("Echo is running.");
			status.Set(string.Join(" ", lines), dot);

			if (installed) LoadColours();
		}

		// ---- Install / uninstall / launch ----

		void Install()
		{
			if (IsGameRunning()) { Msg("Close Echo first."); return; }
			if (IsSupportedGame(P("echovr.exe")) == false &&
				MessageBox.Show(this, "This echovr.exe is a different build than DiscGlow was made for, so it will not change anything. Install anyway?",
					"DiscGlow", MessageBoxButtons.YesNo, MessageBoxIcon.Warning) != DialogResult.Yes)
				return;
			try
			{
				string dll = P(DllName), chain = P(ChainName);
				if (File.Exists(dll) && !IsDiscGlow(dll))
				{
					// Another mod installed as dinput8.dll: keep it and let DiscGlow load it
					if (File.Exists(chain)) { Msg("Both dinput8.dll and dinput8.chain.dll belong to other mods. Remove one of them first."); return; }
					File.Move(dll, chain);
				}
				File.WriteAllBytes(dll, EmbeddedDllBytes());

				var ini = new IniFile(P(IniName));
				ini.SetDefault("DiscGlow", "PersonalDisc", "1");
				foreach (var picker in _pickers)
				{
					ini.SetDefault("DiscGlow", picker.Key, Format(picker.Default));
					ini.SetDefault("DiscGlow", picker.SaturationKey, "1");
					ini.SetDefault("DiscGlow", picker.EffectKey, "none");
					ini.SetDefault("DiscGlow", picker.SpeedKey, "2");
				}
				ini.SetDefault("DiscGlow", "StickyTeamColour", "1");
				ini.Save();

				RefreshAll();
				Msg(File.Exists(chain) ? "Installed. Your other dinput8 mod (" + ChainName + ") is loaded by DiscGlow, so it keeps working." : "Installed. Start Echo and pick your colours here.");
			}
			catch (Exception ex) { Msg("Install failed: " + ex.Message); }
		}

		void Uninstall()
		{
			if (IsGameRunning()) { Msg("Close Echo first."); return; }
			try
			{
				string dll = P(DllName), chain = P(ChainName);
				if (IsDiscGlow(dll)) File.Delete(dll);
				if (!File.Exists(dll) && File.Exists(chain)) File.Move(chain, dll); // Give the other mod its name back
				RefreshAll();
				Msg("Uninstalled. Your colour settings (" + IniName + ") were kept.");
			}
			catch (Exception ex) { Msg("Uninstall failed: " + ex.Message); }
		}

		void Launch()
		{
			if (IsGameRunning()) { Msg("Echo is already running."); return; }
			try { Process.Start(new ProcessStartInfo(P("echovr.exe")) { WorkingDirectory = _folder, UseShellExecute = true }); }
			catch (Exception ex) { Msg("Could not start Echo: " + ex.Message); }
		}

		// ---- Colours ----

		static string Format(float[] rgb)
		{
			return string.Join(" ", rgb.Select(v => v.ToString("0.###", CultureInfo.InvariantCulture)));
		}
		static float[] Parse(string value, float[] fallback)
		{
			if (string.IsNullOrEmpty(value)) return fallback;
			var parts = value.Split(new[] { ' ', '\t' }, StringSplitOptions.RemoveEmptyEntries);
			var result = new float[3];
			for (int i = 0; i < 3; i++)
				if (parts.Length != 3 || !float.TryParse(parts[i], NumberStyles.Float, CultureInfo.InvariantCulture, out result[i])) return fallback;
			return result;
		}

		void LoadColours()
		{
			_loading = true;
			try
			{
				var ini = new IniFile(P(IniName));
				foreach (var picker in _pickers)
				{
					float saturation;
					if (!float.TryParse(ini.Get("DiscGlow", picker.SaturationKey), NumberStyles.Float, CultureInfo.InvariantCulture, out saturation)) saturation = 1f;
					picker.SetValue(Parse(ini.Get("DiscGlow", picker.Key), picker.Default), saturation);
					int effect = Array.IndexOf(ColourPicker.EffectNames, (ini.Get("DiscGlow", picker.EffectKey) ?? "none").ToLowerInvariant());
					picker.Effect.Selected = Math.Max(0, effect);
					float speed;
					picker.Speed.Value = float.TryParse(ini.Get("DiscGlow", picker.SpeedKey), NumberStyles.Float, CultureInfo.InvariantCulture, out speed) ? (decimal)Math.Round(speed, 1) : 2m;
					picker.Speed.Enabled = picker.Effect.Selected != 0;
					if (picker.Effect.Selected == 0) picker.Swatch.SetColour(picker.Effective);
				}
				_pickers[0].Enable.Checked = ini.Get("DiscGlow", "PersonalDisc") != "0";
				foreach (var s in _pickers[0].Sliders) s.Enabled = _pickers[0].Enable.Checked;
				_pickers[0].Saturation.Enabled = _pickers[0].Enable.Checked;
				_pickers[0].Effect.Enabled = _pickers[0].Enable.Checked;
				_pickers[0].Speed.Enabled = _pickers[0].Enable.Checked && _pickers[0].Effect.Selected != 0;
				chkSticky.Checked = ini.Get("DiscGlow", "StickyTeamColour") != "0";
			}
			finally { _loading = false; }
		}

		void ColoursChanged()
		{
			if (_loading) return;
			_saveTimer.Stop();
			_saveTimer.Start();
		}

		void SaveColours()
		{
			if (_folder == null || !IsDiscGlow(P(DllName))) return;
			try
			{
				var ini = new IniFile(P(IniName));
				foreach (var picker in _pickers)
				{
					ini.Set("DiscGlow", picker.Key, Format(picker.Value));
					ini.Set("DiscGlow", picker.SaturationKey, picker.Saturation.Value.ToString("0.##", CultureInfo.InvariantCulture));
					ini.Set("DiscGlow", picker.EffectKey, ColourPicker.EffectNames[picker.Effect.Selected]);
					ini.Set("DiscGlow", picker.SpeedKey, picker.Speed.Value.ToString("0.#", CultureInfo.InvariantCulture));
				}
				ini.Set("DiscGlow", "PersonalDisc", _pickers[0].Enable.Checked ? "1" : "0");
				ini.Set("DiscGlow", "StickyTeamColour", chkSticky.Checked ? "1" : "0");
				ini.Save();
			}
			catch (IOException) { _saveTimer.Start(); } // The game may be reading it; try again shortly
			catch (Exception ex) { Msg("Could not save the colours: " + ex.Message); }
		}

		void Msg(string text) { MessageBox.Show(this, text, "DiscGlow"); }
	}
}
