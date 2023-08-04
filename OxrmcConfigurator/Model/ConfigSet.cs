using System.Collections.Generic;
using System.Linq;
using IniParser;
using IniParser.Model;

namespace OxrmcConfigurator.Model
{
	internal class ConfigSet
	{
		private readonly SortedDictionary<string, ConfigSection> _sections;
		
		public ConfigSet(string application)
		{
			_sections = new SortedDictionary<string, ConfigSection>();
			ParseConfigFile(application);
		}

		public void ParseConfigFile(string application)
		{
			var parser = new FileIniDataParser();
			string path = ConfigService.appDataPath + application + ".ini";
			IniData _data = parser.ReadFile(path);
			foreach (var section in _data.Sections)
			{
				_sections.Add(section.SectionName, new ConfigSection(application, section));
			}
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
}
