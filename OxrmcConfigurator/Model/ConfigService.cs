using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using Serilog;

namespace OxrmcConfigurator.Model
{
	internal class ConfigService
	{
		public static string appDataPath = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData) + @"\OpenXR-MotionCompensation\";
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
			foreach (var file in Directory.GetFiles(appDataPath, "*.ini"))
			{
				var application = Path.GetFileName(file);
				application = application[..application.LastIndexOf('.')];
				_configSets.Add(application, new ConfigSet(application));
			}
		}

		public List<string> GetAppNames()
		{
			var configs = _configSets.Keys.ToList();
			for (int i = 0; i < configs.Count; i++)
			{
				if (!configs[i].Equals("OpenXR-MotionCompensation")) continue;
				configs.RemoveAt(i);
				return configs;
			}
			Log.Error("No default config file found at: " + appDataPath);
			return new List<string>();
		}

		public ConfigSet? GetConfigSet(string name)
		{
			return _configSets.TryGetValue(name, out var configSet) ? configSet : null;
		}

		public bool SaveConfigSet(string name, ref ConfigSet configSet)
		{
			if (!_configSets.ContainsKey(name)) return false;
			if (!configSet.SaveSet()) return false;
			_configSets[name] = configSet;
			return true;
		}
	}
}
