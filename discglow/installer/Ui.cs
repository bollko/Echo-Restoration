// Shared UI pieces (theme, custom controls, INI editing), adapted from the EchoVRShade app so both tools look alike.
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Windows.Forms;

namespace DiscGlowSetup
{
	// Minimal INI editor that keeps unknown keys, sections and ordering intact (the DLL and the user edit these files too)
	class IniFile
	{
		readonly string _path;
		readonly List<string> _lines;

		public IniFile(string path)
		{
			_path = path;
			_lines = File.Exists(path) ? File.ReadAllLines(path).ToList() : new List<string>();
		}

		static string SectionOf(string line)
		{
			string t = line.Trim();
			return t.StartsWith("[") && t.EndsWith("]") ? t.Substring(1, t.Length - 2) : null;
		}

		int Find(string section, string key)
		{
			string current = "";
			for (int i = 0; i < _lines.Count; i++)
			{
				string s = SectionOf(_lines[i]);
				if (s != null) { current = s; continue; }
				if (!string.Equals(current, section, StringComparison.OrdinalIgnoreCase)) continue;
				int eq = _lines[i].IndexOf('=');
				if (eq > 0 && string.Equals(_lines[i].Substring(0, eq).Trim(), key, StringComparison.OrdinalIgnoreCase))
					return i;
			}
			return -1;
		}

		public string Get(string section, string key)
		{
			int i = Find(section, key);
			return i < 0 ? null : _lines[i].Substring(_lines[i].IndexOf('=') + 1).Trim();
		}

		public void Set(string section, string key, string value)
		{
			string line = key + "=" + value;
			int i = Find(section, key);
			if (i >= 0) { _lines[i] = line; return; }

			int sectionStart = -1;
			if (section.Length == 0)
				sectionStart = 0;
			else
				for (int k = 0; k < _lines.Count; k++)
					if (string.Equals(SectionOf(_lines[k]), section, StringComparison.OrdinalIgnoreCase)) { sectionStart = k + 1; break; }

			if (sectionStart < 0)
			{
				if (_lines.Count > 0 && _lines[_lines.Count - 1].Trim().Length != 0) _lines.Add("");
				_lines.Add("[" + section + "]");
				_lines.Add(line);
				return;
			}

			int sectionEnd = _lines.Count;
			for (int k = sectionStart; k < _lines.Count; k++)
				if (SectionOf(_lines[k]) != null) { sectionEnd = k; break; }
			int insertAt = sectionEnd;
			while (insertAt > sectionStart && _lines[insertAt - 1].Trim().Length == 0) insertAt--;
			_lines.Insert(insertAt, line);
		}

		public void SetDefault(string section, string key, string value)
		{
			if (Get(section, key) == null) Set(section, key, value);
		}

		public static void WriteAllTextAtomic(string path, string text)
		{
			string temp = path + ".tmp";
			File.WriteAllText(temp, text);
			if (File.Exists(path))
				File.Replace(temp, path, null);
			else
				File.Move(temp, path);
		}

		public void Save()
		{
			// Write to a temporary file and swap it in, so the DLL never reads a half-written file while live reloading
			string temp = _path + ".tmp";
			File.WriteAllLines(temp, _lines);
			if (File.Exists(_path))
				File.Replace(temp, _path, null);
			else
				File.Move(temp, _path);
		}
	}

	static class Theme
	{
		public static readonly Color Back = Color.FromArgb(22, 24, 29);
		public static readonly Color Card = Color.FromArgb(32, 35, 42);
		public static readonly Color Border = Color.FromArgb(48, 52, 62);
		public static readonly Color Track = Color.FromArgb(58, 63, 74);
		public static readonly Color Text = Color.FromArgb(232, 234, 237);
		public static readonly Color Muted = Color.FromArgb(150, 156, 166);
		public static readonly Color Disabled = Color.FromArgb(95, 100, 110);
		public static readonly Color Accent = Color.FromArgb(255, 138, 61);
		public static readonly Color AccentHover = Color.FromArgb(255, 164, 104);
		public static readonly Color Good = Color.FromArgb(80, 200, 120);
		public static readonly Color Warn = Color.FromArgb(240, 185, 60);

		public static readonly Font Body = new Font("Segoe UI", 9.5f);
		public static readonly Font Small = new Font("Segoe UI", 8.5f);
		public static readonly Font Semibold = new Font("Segoe UI Semibold", 10f);
		public static readonly Font CardTitle = new Font("Segoe UI Semibold", 11.5f);
		public static readonly Font Title = new Font("Segoe UI Semibold", 17f);

		public static GraphicsPath Round(RectangleF r, float radius)
		{
			var p = new GraphicsPath();
			float d = radius * 2;
			p.AddArc(r.X, r.Y, d, d, 180, 90);
			p.AddArc(r.Right - d, r.Y, d, d, 270, 90);
			p.AddArc(r.Right - d, r.Bottom - d, d, d, 0, 90);
			p.AddArc(r.X, r.Bottom - d, d, d, 90, 90);
			p.CloseFigure();
			return p;
		}
	}

	abstract class ThemedControl : Control
	{
		protected ThemedControl()
		{
			SetStyle(ControlStyles.AllPaintingInWmPaint | ControlStyles.UserPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.ResizeRedraw, true);
			BackColor = Theme.Card;
			ForeColor = Theme.Text;
			Font = Theme.Body;
		}
		protected override void OnEnabledChanged(EventArgs e) { base.OnEnabledChanged(e); Invalidate(); }
		protected override void OnTextChanged(EventArgs e) { base.OnTextChanged(e); Invalidate(); }
	}

	class Card : Panel
	{
		public string Title;
		public Card()
		{
			SetStyle(ControlStyles.AllPaintingInWmPaint | ControlStyles.UserPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.ResizeRedraw, true);
			BackColor = Theme.Back;
		}
		protected override void OnPaint(PaintEventArgs e)
		{
			var g = e.Graphics;
			g.SmoothingMode = SmoothingMode.AntiAlias;
			g.Clear(Theme.Back);
			using (var path = Theme.Round(new RectangleF(0.5f, 0.5f, Width - 1.5f, Height - 1.5f), 12))
			using (var fill = new SolidBrush(Theme.Card))
			using (var pen = new Pen(Theme.Border))
			{
				g.FillPath(fill, path);
				g.DrawPath(pen, path);
			}
			TextRenderer.DrawText(g, Title, Theme.CardTitle, new Point(16, 14), Theme.Text);
		}
	}

	class FlatButton : ThemedControl
	{
		public bool Primary;
		bool _hover;
		public FlatButton() { Cursor = Cursors.Hand; }
		protected override void OnMouseEnter(EventArgs e) { _hover = true; Invalidate(); base.OnMouseEnter(e); }
		protected override void OnMouseLeave(EventArgs e) { _hover = false; Invalidate(); base.OnMouseLeave(e); }
		protected override void OnKeyDown(KeyEventArgs e)
		{
			if (e.KeyCode == Keys.Space || e.KeyCode == Keys.Enter) OnClick(EventArgs.Empty);
			base.OnKeyDown(e);
		}
		protected override void OnPaint(PaintEventArgs e)
		{
			var g = e.Graphics;
			g.SmoothingMode = SmoothingMode.AntiAlias;
			g.Clear(Theme.Card);
			Color fill, text, border;
			if (!Enabled) { fill = Theme.Card; text = Theme.Disabled; border = Theme.Border; }
			else if (Primary) { fill = _hover ? Theme.AccentHover : Theme.Accent; text = Color.FromArgb(28, 20, 14); border = fill; }
			else { fill = _hover ? Theme.Track : Theme.Card; text = Theme.Text; border = Theme.Track; }
			using (var path = Theme.Round(new RectangleF(0.5f, 0.5f, Width - 1.5f, Height - 1.5f), 7))
			using (var b = new SolidBrush(fill))
			using (var p = new Pen(border))
			{
				g.FillPath(b, path);
				g.DrawPath(p, path);
			}
			TextRenderer.DrawText(g, Text, Primary ? Theme.Semibold : Theme.Body, ClientRectangle, text,
				TextFormatFlags.HorizontalCenter | TextFormatFlags.VerticalCenter);
		}
	}

	class ToggleSwitch : ThemedControl
	{
		bool _checked;
		public string Description, Badge;
		public event EventHandler CheckedChanged;
		public ToggleSwitch() { Cursor = Cursors.Hand; }
		public bool Checked
		{
			get { return _checked; }
			set
			{
				if (_checked == value) return;
				_checked = value;
				Invalidate();
				if (CheckedChanged != null) CheckedChanged(this, EventArgs.Empty);
			}
		}
		protected override void OnClick(EventArgs e) { Checked = !Checked; base.OnClick(e); }
		protected override void OnKeyDown(KeyEventArgs e)
		{
			if (e.KeyCode == Keys.Space) Checked = !Checked;
			base.OnKeyDown(e);
		}
		protected override void OnPaint(PaintEventArgs e)
		{
			var g = e.Graphics;
			g.SmoothingMode = SmoothingMode.AntiAlias;
			g.Clear(Theme.Card);
			var track = new RectangleF(0.5f, 2.5f, 38, 20);
			Color trackColor = !Enabled ? Theme.Border : (_checked ? Theme.Accent : Theme.Track);
			using (var path = Theme.Round(track, 10))
			using (var b = new SolidBrush(trackColor))
				g.FillPath(b, path);
			float knobX = _checked ? track.Right - 18 : track.X + 2;
			using (var b = new SolidBrush(Enabled ? Color.White : Theme.Muted))
				g.FillEllipse(b, knobX, track.Y + 2, 16, 16);

			Color text = Enabled ? Theme.Text : Theme.Disabled;
			if (string.IsNullOrEmpty(Description))
			{
				int badgeWidth = 0;
				if (!string.IsNullOrEmpty(Badge))
				{
					badgeWidth = TextRenderer.MeasureText(Badge, Theme.Small).Width + 8;
					TextRenderer.DrawText(g, Badge, Theme.Small, new Rectangle(Width - badgeWidth, 0, badgeWidth, 25), Enabled ? Theme.Muted : Theme.Disabled,
						TextFormatFlags.Right | TextFormatFlags.VerticalCenter);
				}
				TextRenderer.DrawText(g, Text, Theme.Semibold, new Rectangle(50, 0, Width - 50 - badgeWidth, 25), text,
					TextFormatFlags.VerticalCenter | TextFormatFlags.EndEllipsis);
			}
			else
			{
				TextRenderer.DrawText(g, Text, Theme.Semibold, new Point(50, 3), text);
				TextRenderer.DrawText(g, Description, Theme.Small, new Rectangle(50, 24, Width - 50, Height - 24),
					Enabled ? Theme.Muted : Theme.Disabled, TextFormatFlags.WordBreak);
			}
		}
	}

	class Slider : ThemedControl
	{
		decimal _value;
		public decimal Minimum = 0, Maximum = 1, Step = 0.01m;
		public int Decimals = 2;
		public Color FillColor = Theme.Accent;
		public event EventHandler ValueChanged;
		const int Pad = 8;

		public Slider() { Cursor = Cursors.Hand; }

		public decimal Value
		{
			get { return _value; }
			set
			{
				value = Math.Max(Minimum, Math.Min(Maximum, value));
				if (_value == value) return;
				_value = value;
				Invalidate();
				if (ValueChanged != null) ValueChanged(this, EventArgs.Empty);
			}
		}

		void SetFromMouse(int x)
		{
			float frac = Math.Max(0f, Math.Min(1f, (x - Pad) / (float)(Width - 2 * Pad)));
			decimal raw = Minimum + (decimal)frac * (Maximum - Minimum);
			Value = Minimum + Math.Round((raw - Minimum) / Step) * Step;
		}
		protected override void OnMouseDown(MouseEventArgs e) { Focus(); if (e.Button == MouseButtons.Left) SetFromMouse(e.X); base.OnMouseDown(e); }
		protected override void OnMouseMove(MouseEventArgs e) { if (e.Button == MouseButtons.Left) SetFromMouse(e.X); base.OnMouseMove(e); }
		protected override void OnMouseWheel(MouseEventArgs e) { Value += e.Delta > 0 ? Step : -Step; base.OnMouseWheel(e); }
		protected override bool IsInputKey(Keys keyData) { return keyData == Keys.Left || keyData == Keys.Right || base.IsInputKey(keyData); }
		protected override void OnKeyDown(KeyEventArgs e)
		{
			if (e.KeyCode == Keys.Left) Value -= Step;
			if (e.KeyCode == Keys.Right) Value += Step;
			base.OnKeyDown(e);
		}

		protected override void OnPaint(PaintEventArgs e)
		{
			var g = e.Graphics;
			g.SmoothingMode = SmoothingMode.AntiAlias;
			g.Clear(Theme.Card);

			TextRenderer.DrawText(g, Text, Theme.Body, new Point(Pad - 3, 0), Enabled ? Theme.Muted : Theme.Disabled);
			string valueText = _value.ToString("F" + Decimals, CultureInfo.InvariantCulture);
			TextRenderer.DrawText(g, valueText, Theme.Semibold, new Rectangle(0, 0, Width - Pad + 3, 20), Enabled ? Theme.Text : Theme.Disabled,
				TextFormatFlags.Right);

			float frac = Maximum > Minimum ? (float)((_value - Minimum) / (Maximum - Minimum)) : 0f;
			float y = 30, left = Pad, right = Width - Pad;
			float thumbX = left + frac * (right - left);
			using (var path = Theme.Round(new RectangleF(left, y - 2, right - left, 4), 2))
			using (var b = new SolidBrush(Theme.Track))
				g.FillPath(b, path);
			if (thumbX - left > 1)
				using (var path = Theme.Round(new RectangleF(left, y - 2, thumbX - left, 4), 2))
				using (var b = new SolidBrush(Enabled ? FillColor : Theme.Disabled))
					g.FillPath(b, path);
			using (var b = new SolidBrush(Enabled ? Color.White : Theme.Muted))
				g.FillEllipse(b, thumbX - 7, y - 7, 14, 14);
			if (Focused)
				using (var p = new Pen(Theme.Accent, 2))
					g.DrawEllipse(p, thumbX - 8, y - 8, 16, 16);
		}
		protected override void OnGotFocus(EventArgs e) { Invalidate(); base.OnGotFocus(e); }
		protected override void OnLostFocus(EventArgs e) { Invalidate(); base.OnLostFocus(e); }
	}

	class StatusLine : ThemedControl
	{
		Color _dot = Theme.Muted;
		public void Set(string text, Color dot) { _dot = dot; Text = text; }
		protected override void OnPaint(PaintEventArgs e)
		{
			var g = e.Graphics;
			g.SmoothingMode = SmoothingMode.AntiAlias;
			g.Clear(Theme.Card);
			using (var b = new SolidBrush(_dot))
				g.FillEllipse(b, 2, 6, 9, 9);
			TextRenderer.DrawText(g, Text, Theme.Body, new Rectangle(18, 0, Width - 18, Height), Theme.Text, TextFormatFlags.WordBreak);
		}
	}

	// A row of options where one is selected (like a tab strip)
	class Segmented : ThemedControl
	{
		public string[] Options = new string[0];
		int _selected;
		public event EventHandler SelectedChanged;
		public Segmented() { Cursor = Cursors.Hand; }
		public int Selected
		{
			get { return _selected; }
			set
			{
				if (_selected == value) return;
				_selected = value;
				Invalidate();
				if (SelectedChanged != null) SelectedChanged(this, EventArgs.Empty);
			}
		}
		protected override void OnMouseDown(MouseEventArgs e)
		{
			if (Enabled && Options.Length > 0) Selected = Math.Max(0, Math.Min(Options.Length - 1, e.X * Options.Length / Width));
			base.OnMouseDown(e);
		}
		protected override void OnPaint(PaintEventArgs e)
		{
			var g = e.Graphics;
			g.SmoothingMode = SmoothingMode.AntiAlias;
			g.Clear(Theme.Card);
			using (var path = Theme.Round(new RectangleF(0.5f, 0.5f, Width - 1.5f, Height - 1.5f), 7))
			using (var b = new SolidBrush(Theme.Back))
			using (var p = new Pen(Theme.Border))
			{
				g.FillPath(b, path);
				g.DrawPath(p, path);
			}
			float w = (Width - 4) / (float)Math.Max(1, Options.Length);
			for (int i = 0; i < Options.Length; i++)
			{
				var r = new RectangleF(2 + i * w, 2, w, Height - 4);
				if (i == _selected)
					using (var path = Theme.Round(r, 5))
					using (var b = new SolidBrush(Enabled ? Theme.Accent : Theme.Track))
						g.FillPath(b, path);
				Color text = !Enabled ? Theme.Disabled : i == _selected ? Color.FromArgb(28, 20, 14) : Theme.Muted;
				TextRenderer.DrawText(g, Options[i], Theme.Small, Rectangle.Round(r), text, TextFormatFlags.HorizontalCenter | TextFormatFlags.VerticalCenter);
			}
		}
	}
}
