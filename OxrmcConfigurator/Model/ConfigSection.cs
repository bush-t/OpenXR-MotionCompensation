using System;
using System.Collections.Generic;
using System.Linq;
using IniParser.Model;
using Serilog;

namespace OxrmcConfigurator.Model;

internal class ConfigSection
{
	public ConfigSection(string application)
	{
		Application = application;
		_entries = new SortedDictionary<string, ConfigEntry>();
	}
	public string Application { get; set; }
	private SortedDictionary<string, ConfigEntry> _entries;

	public void ParseSection(SectionData section)
	{
		var sectionName = section.SectionName;
		ConfigEntry entry = new ConfigEntry("", "", "", "");
		try
		{
			foreach (var key in section.Keys)
			{
				entry = new ConfigEntry(Application, sectionName, key.KeyName, key.Value);
				_entries.Add(key.KeyName, entry);
			}
		}
		catch (ArgumentException)
		{
			Log.Error("found duplicate entry: " + entry.ToString());
		}
	}

	public ConfigEntry? TryGetEntry(string name)
	{
		return _entries.TryGetValue(name, out var entry) ? entry : null;
	}

	public void SetEntry(ConfigEntry entry)
	{
		_entries[entry.Key] = entry;
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
