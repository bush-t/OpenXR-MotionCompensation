using System.Collections.Generic;
using System.Linq;
using IniParser;
using IniParser.Model;

namespace OxrmcConfigurator.Model;

internal class ConfigSet
{
	public string Application { get; set; }
	private readonly SortedDictionary<string, ConfigSection> _sections;

	public ConfigSet(string application)
	{
		Application = application;
		_sections = new SortedDictionary<string, ConfigSection>();
		ParseConfigFile();
	}

	public void ParseConfigFile()
	{
		var parser = new FileIniDataParser();
		var path = ConfigService.appDataPath + Application + ".ini";
		var data = parser.ReadFile(path);
		foreach (var sectionData in data.Sections)
		{
			ConfigSection section = new(Application);
			section.ParseSection(sectionData);
			_sections.Add(sectionData.SectionName, section);
		}
	}

	public ConfigSection? TryGetSection(string name)
	{
		return _sections.TryGetValue(name, out var section) ? section : null;
	}

	public void SetEntry(ConfigEntry entry)
	{
		ConfigSection section = _sections.TryGetValue(entry.Section, out var configSection) ? configSection : new ConfigSection(Application);
		section.SetEntry(entry);
		_sections[entry.Section] = section;
	}
	public bool IsModified()
	{
		return _sections.Values.Any(section => section.IsModified());
	}
	public bool SaveSet()
	{
		var success = true;
		foreach (var section in _sections.Where(section => !section.Value.SaveSection()))
		{
			success = false;
		}
		return success;
	}


}
