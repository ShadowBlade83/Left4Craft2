package dev.hl2craft.client;

import dev.hl2craft.HL2Craft;
import java.io.BufferedReader;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardOpenOption;
import java.util.ArrayList;
import java.util.List;
import java.util.regex.Matcher;
import java.util.regex.Pattern;
import net.minecraft.client.Minecraft;

public final class HL2SetupManager {
	private static final Path CONFIG_PATH = Path.of("hl2craft_path.txt");
	private static Path cachedHL2Dir = null;

	public static Path findHL2Dir() {
		if (cachedHL2Dir != null && Files.exists(cachedHL2Dir.resolve("left4dead2.exe"))) {
			return cachedHL2Dir;
		}

		// 1. Saved path from config
		try {
			if (Files.exists(CONFIG_PATH)) {
				String line = Files.readString(CONFIG_PATH).trim();
				if (!line.isEmpty()) {
					Path p = Path.of(line);
					if (Files.exists(p.resolve("left4dead2.exe"))) {
						cachedHL2Dir = p;
						return p;
					}
				}
			}
		} catch (Exception ignored) {}

		// 2. System property
		String sysProp = System.getProperty("hl2.path");
		if (sysProp != null && !sysProp.isBlank()) {
			Path p = Path.of(sysProp.trim());
			if (Files.exists(p.resolve("left4dead2.exe"))) {
				setHL2Dir(p);
				return p;
			}
		}

		// 3. Scan Steam libraries
		List<Path> steamRoots = new ArrayList<>();

		// Query Windows registry for SteamPath
		try {
			Process proc = new ProcessBuilder("reg", "query", "HKCU\\Software\\Valve\\Steam", "/v", "SteamPath").start();
			try (BufferedReader reader = new BufferedReader(new InputStreamReader(proc.getInputStream()))) {
				String line;
				while ((line = reader.readLine()) != null) {
					if (line.contains("SteamPath") && line.contains("REG_SZ")) {
						String val = line.substring(line.indexOf("REG_SZ") + 6).trim();
						Path p = Path.of(val);
						if (Files.isDirectory(p)) steamRoots.add(p);
					}
				}
			}
		} catch (Exception ignored) {}

		// Standard drive locations
		for (char d = 'C'; d <= 'Z'; d++) {
			String drive = d + ":";
			steamRoots.add(Path.of(drive, "Program Files (x86)", "Steam"));
			steamRoots.add(Path.of(drive, "Program Files", "Steam"));
			steamRoots.add(Path.of(drive, "Steam"));
			steamRoots.add(Path.of(drive, "SteamLibrary"));
			steamRoots.add(Path.of(drive, "games", "Steam"));
			steamRoots.add(Path.of(drive, "Games", "Steam"));
		}

		Pattern vdfPathPattern = Pattern.compile("\"path\"\\s+\"([^\"]+)\"");
		for (Path root : steamRoots) {
			if (!Files.isDirectory(root)) continue;

			// Direct check
			Path directHl2 = root.resolve("steamapps/common/Left 4 Dead 2");
			if (Files.exists(directHl2.resolve("left4dead2.exe"))) {
				setHL2Dir(directHl2);
				return directHl2;
			}

			// libraryfolders.vdf
			Path vdf = root.resolve("steamapps/libraryfolders.vdf");
			if (Files.exists(vdf)) {
				try {
					List<String> lines = Files.readAllLines(vdf);
					for (String line : lines) {
						Matcher m = vdfPathPattern.matcher(line);
						if (m.find()) {
							String libPathStr = m.group(1).replace("\\\\", "\\");
							Path libHl2 = Path.of(libPathStr, "steamapps", "common", "Left 4 Dead 2");
							if (Files.exists(libHl2.resolve("left4dead2.exe"))) {
								setHL2Dir(libHl2);
								return libHl2;
							}
						}
					}
				} catch (Exception ignored) {}
			}
		}

		return null;
	}

	public static void setHL2Dir(Path dir) {
		cachedHL2Dir = dir;
		try {
			Files.writeString(CONFIG_PATH, dir.toAbsolutePath().toString(), StandardOpenOption.CREATE, StandardOpenOption.TRUNCATE_EXISTING);
		} catch (Exception ignored) {}
	}

	public static boolean isReShadeInstalled(Path hl2Dir) {
		if (hl2Dir == null) return false;
		return Files.exists(hl2Dir.resolve("bin/d3d9.dll")) || Files.exists(hl2Dir.resolve("d3d9.dll"));
	}

	public static byte[] getEmbeddedAddonBytes() {
		try (InputStream in = HL2SetupManager.class.getResourceAsStream("/assets/hl2craft/HL2Minecraft.addon32")) {
			if (in != null) return in.readAllBytes();
		} catch (Exception ignored) {}
		return null;
	}

	public static byte[] getEmbeddedShaderBytes() {
		try (InputStream in = HL2SetupManager.class.getResourceAsStream("/assets/hl2craft/HL2Minecraft.fx")) {
			if (in != null) return in.readAllBytes();
		} catch (Exception ignored) {}
		return null;
	}

	public static boolean isAddonUpToDate(Path hl2Dir) {
		if (hl2Dir == null) return false;
		byte[] addon = getEmbeddedAddonBytes();
		if (addon == null) return true;

		Path target = hl2Dir.resolve("HL2Minecraft.addon32");
		if (!Files.exists(target)) return false;

		try {
			return Files.size(target) == addon.length;
		} catch (Exception e) {
			return false;
		}
	}

	public static boolean installOrUpdateAddon(Path hl2Dir) {
		if (hl2Dir == null) return false;
		byte[] addon = getEmbeddedAddonBytes();
		if (addon == null) return false;

		try {
			// L4D2: ReShade's d3d9.dll sits next to left4dead2.exe and looks for add-ons in that folder
			Path target2 = hl2Dir.resolve("HL2Minecraft.addon32");
			Files.write(target2, addon, StandardOpenOption.CREATE, StandardOpenOption.TRUNCATE_EXISTING);
			HL2Craft.LOG.info("Left4Craft: Installed/Updated HL2Minecraft.addon32 in {}", hl2Dir);

			// The VScript half (barricades the infected wear down): left4dead2/scripts/vscripts/left4craft.nut
			try (InputStream nut = HL2SetupManager.class.getResourceAsStream("/assets/hl2craft/left4craft.nut")) {
				if (nut != null) {
					Path nutTarget = hl2Dir.resolve("left4dead2/scripts/vscripts/left4craft.nut");
					Files.createDirectories(nutTarget.getParent());
					Files.write(nutTarget, nut.readAllBytes(), StandardOpenOption.CREATE, StandardOpenOption.TRUNCATE_EXISTING);
				}
			}

			byte[] shader = getEmbeddedShaderBytes();
			if (shader != null) {
				Path shaderTarget = hl2Dir.resolve("reshade-shaders/Shaders/HL2Minecraft.fx");
				Files.createDirectories(shaderTarget.getParent());
				Files.write(shaderTarget, shader, StandardOpenOption.CREATE, StandardOpenOption.TRUNCATE_EXISTING);
			}

			// The compositor effect draws Minecraft's hand, HUD and screens over L4D2: switch its technique on in the
			// preset ReShade uses, unless the player already chose techniques there
			Path preset = hl2Dir.resolve("ReShadePreset.ini");
			String presetText = Files.exists(preset) ? Files.readString(preset) : "";
			if (!presetText.contains("Techniques=")) {
				Files.writeString(preset, "Techniques=HL2Minecraft@HL2Minecraft.fx\nTechniqueSorting=HL2Minecraft@HL2Minecraft.fx\n" + presetText);
				HL2Craft.LOG.info("Left4Craft: enabled HL2Minecraft.fx in {}", preset);
			}

						// Ensure ReShade.ini does not disable addon
			Path iniPath = hl2Dir.resolve("bin/ReShade.ini");
			if (!Files.exists(iniPath)) iniPath = hl2Dir.resolve("ReShade.ini");
			if (Files.exists(iniPath)) {
				String iniContent = Files.readString(iniPath);
				if (iniContent.contains("DisabledAddons=HL2Minecraft")) {
					iniContent = iniContent.replace("DisabledAddons=HL2Minecraft", "DisabledAddons=");
					Files.writeString(iniPath, iniContent);
				}
			}

			// (no autoexec: L4D2's own config is left alone)

			return true;
		} catch (Exception e) {
			HL2Craft.LOG.error("HL2Craft: Failed to install addon: {}", e.getMessage());
			return false;
		}
	}

	public static boolean isHL2Running() {
		try {
			return ProcessHandle.allProcesses().anyMatch(p -> {
				String cmd = p.info().command().orElse("").toLowerCase();
				return cmd.endsWith("left4dead2.exe");
			});
		} catch (Exception e) {
			return false;
		}
	}

	public static void launchHL2(Path hl2Dir) {
		if (isHL2Running()) return;

		try {
			// -insecure is not optional: the add-on and ReShade are in the process, so VAC must be off. Only local
			// games; never join official servers with this running.
			Path exe = hl2Dir == null ? null : hl2Dir.resolve("left4dead2.exe");
			if (exe != null && Files.exists(exe)) {
				HL2Craft.LOG.info("Left4Craft: launching {} -insecure", exe);
				java.util.List<String> args = new java.util.ArrayList<>(java.util.List.of(exe.toString(), "-game", "left4dead2", "-insecure", "-novid", "-windowed", "-noborder", "-condebug"));
				// -Dleft4craft.map=c1m1_hotel (dev runs): straight into a local game on that map, sv_cheats on
				String map = System.getProperty("left4craft.map", "");
				if (map.matches("[A-Za-z0-9_]+")) {
					args.addAll(java.util.List.of("+sv_cheats", "1", "+map", map));
				}
				new ProcessBuilder(args).directory(hl2Dir.toFile()).start();
			} else {
				HL2Craft.LOG.info("Left4Craft: launching Left 4 Dead 2 through Steam with -insecure");
				new ProcessBuilder("cmd", "/c", "start", "", "steam://run/550//-insecure -novid -windowed -noborder/").start();
			}
		} catch (Exception e) {
			HL2Craft.LOG.error("Left4Craft: launching Left 4 Dead 2 failed: {}", e.getMessage());
		}
	}

	public static void openUrl(String url) {
		try {
			new ProcessBuilder("cmd", "/c", "start", url).start();
		} catch (Exception ignored) {}
	}

	public static Path browseForHL2Folder() {
		try {
			String script = "Add-Type -AssemblyName System.Windows.Forms; $f = New-Object System.Windows.Forms.FolderBrowserDialog; $f.Description = 'Выберите папку Left 4 Dead 2'; if ($f.ShowDialog() -eq [System.Windows.Forms.DialogResult]::OK) { Write-Output $f.SelectedPath }";
			Process proc = new ProcessBuilder("powershell", "-NoProfile", "-Command", script).start();
			try (BufferedReader reader = new BufferedReader(new InputStreamReader(proc.getInputStream()))) {
				String line = reader.readLine();
				if (line != null && !line.isBlank()) {
					Path selected = Path.of(line.trim());
					if (Files.exists(selected.resolve("left4dead2.exe"))) {
						setHL2Dir(selected);
						return selected;
					}
				}
			}
		} catch (Exception ignored) {}
		return null;
	}

	public static void checkAndProceed(Minecraft minecraft, Runnable onComplete) {
		Path hl2Dir = findHL2Dir();
		boolean reshade = isReShadeInstalled(hl2Dir);

		if (hl2Dir == null || !reshade) {
			minecraft.gui.setScreen(new HL2SetupScreen(minecraft, onComplete));
		} else {
			installOrUpdateAddon(hl2Dir);
			if (!isHL2Running()) {
				launchHL2(hl2Dir);
			}
			onComplete.run();
		}
	}
}
