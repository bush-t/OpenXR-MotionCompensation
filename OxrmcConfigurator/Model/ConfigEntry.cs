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
	}

	private string Application { get; }
	private string Section { get; }
	private string Key { get; }
	private string Value { get; set; }
	private bool Modified { get; set; }

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
