
namespace OxrmcConfigurator.Model;

public class ConfigItem
{
	public ConfigItem(string configName)
	{
		name = configName;
		displayName = GetDisplayName(configName);
	}

	private static string GetDisplayName(string configName)
	{
		if (configName == ConfigService.defaultApp)
			return "Default";
		if (configName.StartsWith("OpenComposite_"))
			return configName["OpenComposite_".Length..] + " (OC)";
		return configName;
	}

	public readonly string name;
	public readonly string displayName;

	public override bool Equals(object? o)
	{
		var other = o as ConfigItem;
		return other?.name == name;
	}

	public override int GetHashCode() => name?.GetHashCode() ?? 0;

	public override string ToString() => displayName;
}
