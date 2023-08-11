using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using Serilog;

namespace OxrmcConfigurator.Model;

internal class ConfigService
{
	public static string appDataPath = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData) +
	                                   @"\OpenXR-MotionCompensation\";

	public static string defaultApp = "OpenXR-MotionCompensation";

	private readonly SortedDictionary<string, ConfigSet> _configSets = new();

	public ConfigService()
	{
		ParseConfigFiles();
	}

	[DllImport("kernel32")]
	public static extern long WritePrivateProfileString(string name, string key, string val, string filePath);

	public void ParseConfigFiles()
	{
		_configSets.Clear();
		try
		{
			foreach (var file in Directory.GetFiles(appDataPath, "*.ini"))
			{
				var application = Path.GetFileName(file);
				application = application[..application.LastIndexOf('.')];
				_configSets.Add(application, new ConfigSet(application));
			}
		}
		catch (Exception ex)
		{
			Log.Error("unable to find config files at " + appDataPath + " - " + ex.Message);
		}
	}

	public List<string> GetAppNames()
	{
		var configs = _configSets.Keys.ToList();
		for (int i = 0; i < configs.Count; i++)
		{
			if (!configs[i].Equals(defaultApp)) continue;
			configs.RemoveAt(i);
			return configs;
		}

		Log.Error("No default config file found at: " + appDataPath);
		return new List<string>();
	}

	public ConfigEntry? TryGetEntry(string application, string section, string key)
	{
		var entry = TryGetConfigSet(application)?.TryGetSection(section)?.TryGetEntry(key);
		if (null != entry) return entry;
		entry = TryGetConfigSet(defaultApp)?.TryGetSection(section)?.TryGetEntry(key);
		if (entry != null)
		{
			entry.Default = true;
		}
		return entry;
	}

	public void SetEntry(string application, string sectionName, string key, string value)
	{
		ConfigEntry entry = new(application, sectionName, key, value);
		entry.Modified = true;

		ConfigSet set = _configSets.TryGetValue(application, out var configSet) ? configSet : new ConfigSet(application);
		set.SetEntry(entry);
		_configSets[application] = set;
	}

	private ConfigSet? TryGetConfigSet(string name)
	{
		return _configSets.TryGetValue(name, out var configSet) ? configSet : null;
	}

	public bool IsModified(string application)
	{
		_configSets.TryGetValue(application, out var configSet);
		return null != configSet && configSet.IsModified();
	}

	public bool SaveConfigSet(string name, ref ConfigSet configSet)
	{
		if (!_configSets.ContainsKey(name)) return false;
		if (!configSet.SaveSet()) return false;
		_configSets[name] = configSet;
		return true;
	}
}
