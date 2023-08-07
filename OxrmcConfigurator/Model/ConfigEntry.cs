using Serilog;

namespace OxrmcConfigurator.Model;

internal class ConfigEntry
{

	public ConfigEntry(string application, string section, string key, string value)
	{
		Application = application;
		Section = section;
		Key = key;
		Value = value;
		Modified = false;
		Default = false;
	}

	public string Application { get; }
	public string Section { get; }
	public string Key { get; }
	public string Value { get; set; }
	public bool Modified { get; set; }
	public bool Default { get; set; }

	public override string ToString()
	{
		return Application + ": [" + Section + "] " + Key + " = " + Value;
	}

	public bool SaveEntry()
	{
		if (!Modified) return true;
		var file = ConfigService.appDataPath + "/" + Application + ".INI";
		if (0 != ConfigService.WritePrivateProfileString(Section, Key, Value, file) && !(Modified = false)) return true;
		Log.Error("unable to write " + ToString() + " into file: " + file);
		return false;
	}
}
