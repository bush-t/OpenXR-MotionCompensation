using System;
using System.Collections.Generic;
using System.Linq;
using IniParser.Model;
using Serilog;

namespace OxrmcConfigurator.Model;

internal class ConfigSection
{
	public ConfigSection(string application, SectionData section)
	{
		_entries = new SortedDictionary<string, ConfigEntry>();
		ParseSection(application, section);
	}


	private SortedDictionary<string, ConfigEntry> _entries;

	void ParseSection(string application, SectionData section)
	{
		var sectionName = section.SectionName;
		ConfigEntry entry = new ConfigEntry("", "", "", "");
		try
		{
			foreach (var key in section.Keys)
			{
				entry = new ConfigEntry(application, sectionName, key.KeyName, key.Value);
				_entries.Add(key.KeyName, entry);
			}
		}
		catch (ArgumentException)
		{
			Log.Error("found duplicate entry: " + entry.ToString());
		}
	}

	public ConfigEntry? GetEntry(string name)
	{
		return _entries.TryGetValue(name, out var entry) ? entry : null;
	}

	public bool SaveSection()
	{
		var success = true;
		foreach (var value in _entries.Values.Where(value => !value.SaveEntry()))
		{
			success = false;
		}

		return success;
	}
}
