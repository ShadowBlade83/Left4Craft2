package dev.hl2craft.client.render;

import com.mojang.blaze3d.platform.NativeImage;
import dev.hl2craft.HL2Craft;
import dev.hl2craft.client.mixin.SpriteContentsAccessor;
import dev.hl2craft.client.mixin.TextureAtlasAccessor;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import net.minecraft.client.Minecraft;
import net.minecraft.client.renderer.texture.TextureAtlas;
import net.minecraft.client.renderer.texture.TextureAtlasSprite;
import net.minecraft.data.AtlasIds;
import net.minecraft.resources.Identifier;
import net.minecraft.server.packs.resources.ResourceManager;

/**
 * Combined block and item atlas in BGRA format for Direct3D 9.
 */
public final class SkyAtlas {
	public final int width, height;
	public final ByteBuffer pixels;
	final int blockW, blockH, itemW, itemH;
	final Object blockSprites, itemSprites;

	static final int CRACK_STAGES = 10;
	static final int CRACK_SIZE = 16;
	static final String[] ARROW_TEXTURES = { "arrow", "arrow_tipped", "arrow_spectral" };
	static final int ARROW_X = CRACK_STAGES * CRACK_SIZE;
	static final int ARROW_W = 32;

	static final String[] MOB_TEXTURES = {
		"creeper/creeper",
		"zombie/zombie",
		"skeleton/skeleton",
		"spider/spider",
		"enderman/enderman",
		"pig/pig_temperate",
		"cow/cow_temperate",
		"sheep/sheep",
		"chicken/chicken_temperate",
	};
	static final String[] MOB_FALLBACKS = {
		null,
		null,
		null,
		null,
		null,
		"pig/pig",
		"cow/cow",
		null,
		"chicken",
	};
	static final int MOB_X = 256;
	static final int MOB_SLOT_SIZE = 64;
	private final float[][] mobUvs = new float[MOB_TEXTURES.length][4];

	private static final class Animation {
		NativeImage image;
		int x, y, w, h, pad;
		int[] index, time;
		int cycle, rowSize;
		boolean interpolate;
		long shown = Long.MIN_VALUE;
	}

	private final java.util.List<Animation> animations = new java.util.ArrayList<>();

	private static int nextPowerOfTwo(int v) {
		int n = 1;
		while (n < v) n <<= 1;
		return n;
	}

	private SkyAtlas(TextureAtlas blocks, TextureAtlas items, ResourceManager resources) {
		TextureAtlasAccessor b = (TextureAtlasAccessor) blocks;
		TextureAtlasAccessor i = (TextureAtlasAccessor) items;
		this.blockW = b.hl2craft$width();
		this.blockH = b.hl2craft$height();
		this.itemW = i.hl2craft$width();
		this.itemH = i.hl2craft$height();
		this.width = nextPowerOfTwo(Math.max(Math.max(this.blockW, this.itemW), MOB_X + MOB_TEXTURES.length * MOB_SLOT_SIZE));
		this.height = nextPowerOfTwo(this.blockH + this.itemH + MOB_SLOT_SIZE);
		this.pixels = ByteBuffer.allocateDirect(this.width * this.height * 4).order(ByteOrder.LITTLE_ENDIAN);
		for (TextureAtlasSprite sprite : b.hl2craft$sprites().values()) {
			this.copy(sprite, this.blockW, this.blockH, 0);
		}
		for (TextureAtlasSprite sprite : i.hl2craft$sprites().values()) {
			this.copy(sprite, this.itemW, this.itemH, this.blockH);
		}
		this.copyCracks(resources);
		this.copyArrows(resources);
		this.copyMobs(resources);
		this.blockSprites = b.hl2craft$sprites();
		this.itemSprites = i.hl2craft$sprites();
	}

	public static SkyAtlas build(Minecraft minecraft) {
		var atlases = minecraft.getAtlasManager();
		return new SkyAtlas(atlases.getAtlasOrThrow(AtlasIds.BLOCKS), atlases.getAtlasOrThrow(AtlasIds.ITEMS), minecraft.getResourceManager());
	}

	private void copyCracks(ResourceManager resources) {
		int y0 = this.blockH + this.itemH;
		for (int stage = 0; stage < CRACK_STAGES; stage++) {
			var id = Identifier.withDefaultNamespace("textures/block/destroy_stage_" + stage + ".png");
			var resource = resources.getResource(id);
			if (resource.isEmpty()) {
				continue;
			}
			try (var in = resource.get().open(); NativeImage image = NativeImage.read(in)) {
				for (int y = 0; y < CRACK_SIZE; y++) {
					for (int x = 0; x < CRACK_SIZE; x++) {
						int argb = image.getPixel(x * image.getWidth() / CRACK_SIZE, y * image.getHeight() / CRACK_SIZE);
						int o = ((y0 + y) * this.width + stage * CRACK_SIZE + x) * 4;
						this.pixels.put(o, (byte) argb);
						this.pixels.put(o + 1, (byte) (argb >> 8));
						this.pixels.put(o + 2, (byte) (argb >> 16));
						this.pixels.put(o + 3, (byte) (argb >>> 24));
					}
				}
			} catch (java.io.IOException e) {
				HL2Craft.LOG.warn("HL2Craft: couldn't read {}", id, e);
			}
		}
	}

	private void copyArrows(ResourceManager resources) {
		int y0 = this.blockH + this.itemH;
		for (int t = 0; t < ARROW_TEXTURES.length; t++) {
			var id = Identifier.withDefaultNamespace("textures/entity/projectiles/" + ARROW_TEXTURES[t] + ".png");
			var resource = resources.getResource(id);
			if (resource.isEmpty()) {
				continue;
			}
			try (var in = resource.get().open(); NativeImage image = NativeImage.read(in)) {
				for (int y = 0; y < CRACK_SIZE; y++) {
					for (int x = 0; x < ARROW_W; x++) {
						int argb = image.getPixel(x * image.getWidth() / 32, y * image.getHeight() / 32);
						int o = ((y0 + y) * this.width + ARROW_X + t * ARROW_W + x) * 4;
						this.pixels.put(o, (byte) argb);
						this.pixels.put(o + 1, (byte) (argb >> 8));
						this.pixels.put(o + 2, (byte) (argb >> 16));
						this.pixels.put(o + 3, (byte) (argb >>> 24));
					}
				}
			} catch (java.io.IOException e) {
				HL2Craft.LOG.warn("HL2Craft: couldn't read {}", id, e);
			}
		}
	}

	private void copyMobs(ResourceManager resources) {
		int y0 = this.blockH + this.itemH;
		for (int m = 0; m < MOB_TEXTURES.length; m++) {
			var id = Identifier.withDefaultNamespace("textures/entity/" + MOB_TEXTURES[m] + ".png");
			var resource = resources.getResource(id);
			if (resource.isEmpty() && MOB_FALLBACKS[m] != null) {
				id = Identifier.withDefaultNamespace("textures/entity/" + MOB_FALLBACKS[m] + ".png");
				resource = resources.getResource(id);
			}
			if (resource.isEmpty()) {
				continue;
			}
			try (var in = resource.get().open(); NativeImage image = NativeImage.read(in)) {
				int imgW = image.getWidth();
				int imgH = image.getHeight();
				int dstX = MOB_X + m * MOB_SLOT_SIZE;
				for (int y = 0; y < imgH && y < MOB_SLOT_SIZE; y++) {
					for (int x = 0; x < imgW && x < MOB_SLOT_SIZE; x++) {
						int argb = image.getPixel(x, y);
						int o = ((y0 + y) * this.width + dstX + x) * 4;
						this.pixels.put(o, (byte) argb);
						this.pixels.put(o + 1, (byte) (argb >> 8));
						this.pixels.put(o + 2, (byte) (argb >> 16));
						this.pixels.put(o + 3, (byte) (argb >>> 24));
					}
				}
				this.mobUvs[m] = new float[] {
					(float) dstX / this.width,
					(float) y0 / this.height,
					(float) (dstX + imgW) / this.width,
					(float) (y0 + imgH) / this.height
				};
			} catch (java.io.IOException e) {
				HL2Craft.LOG.warn("HL2Craft: couldn't read mob texture {}", id, e);
			}
		}
	}

	public float u(TextureAtlasSprite s, float u) {
		return u * (isItem(s) ? this.itemW : this.blockW) / this.width;
	}

	public float v(TextureAtlasSprite s, float v) {
		return isItem(s) ? (this.blockH + v * this.itemH) / this.height : v * this.blockH / this.height;
	}

	private static boolean isItem(TextureAtlasSprite sprite) {
		return sprite.atlasLocation().equals(AtlasIds.ITEMS) || sprite.atlasLocation().equals(TextureAtlas.LOCATION_ITEMS);
	}

	public float blockU(float u) {
		return u * this.blockW / this.width;
	}

	public float blockV(float v) {
		return v * this.blockH / this.height;
	}

	public float itemU(float u) {
		return u * this.itemW / this.width;
	}

	public float itemV(float v) {
		return (this.blockH + v * this.itemH) / this.height;
	}

	public static NativeImage image(TextureAtlas atlas) {
		TextureAtlasAccessor a = (TextureAtlasAccessor) atlas;
		int w = a.hl2craft$width(), h = a.hl2craft$height();
		NativeImage out = new NativeImage(w, h, true);
		for (TextureAtlasSprite sprite : a.hl2craft$sprites().values()) {
			NativeImage image = ((SpriteContentsAccessor) sprite.contents()).hl2craft$originalImage();
			int sw = Math.min(sprite.contents().width(), image.getWidth());
			int sh = Math.min(sprite.contents().height(), image.getHeight());
			int imageX = Math.round(sprite.getU0() * w), imageY = Math.round(sprite.getV0() * h);
			int pad = Math.max(0, Math.min(imageX - sprite.getX(), imageY - sprite.getY()));
			for (int y = -pad; y < sh + pad; y++) {
				for (int x = -pad; x < sw + pad; x++) {
					int tx = imageX + x, ty = imageY + y;
					if (tx >= 0 && tx < w && ty >= 0 && ty < h) {
						out.setPixel(tx, ty, image.getPixel(Math.clamp(x, 0, sw - 1), Math.clamp(y, 0, sh - 1)));
					}
				}
			}
		}
		return out;
	}

	public float[] arrowUv(int variant) {
		float x0 = ARROW_X + Math.clamp(variant, 0, ARROW_TEXTURES.length - 1) * ARROW_W, y0 = this.blockH + this.itemH;
		return new float[] {
			x0 / this.width, y0 / this.height, (x0 + 16) / this.width, (y0 + 5) / this.height,
			x0 / this.width, (y0 + 5) / this.height, (x0 + 5) / this.width, (y0 + 10) / this.height,
		};
	}

	public float[] crackUv(int stage) {
		int s = Math.clamp(stage, 0, CRACK_STAGES - 1);
		float y0 = (float) (this.blockH + this.itemH) / this.height, y1 = (float) (this.blockH + this.itemH + CRACK_SIZE) / this.height;
		return new float[] { (float) (s * CRACK_SIZE) / this.width, y0, (float) ((s + 1) * CRACK_SIZE) / this.width, y1 };
	}

	public float[] mobUv(int index) {
		if (index < 0 || index >= this.mobUvs.length) return null;
		return this.mobUvs[index];
	}

	public float[] rect(TextureAtlasSprite s) {
		return new float[] { this.u(s, s.getU0()), this.v(s, s.getV0()), this.u(s, s.getU1()), this.v(s, s.getV1()) };
	}

	public boolean stale(Minecraft minecraft) {
		return false;
	}

	private void copy(TextureAtlasSprite sprite, int atlasW, int atlasH, int yOffset) {
		NativeImage image = ((SpriteContentsAccessor) sprite.contents()).hl2craft$originalImage();
		int w = Math.min(sprite.contents().width(), image.getWidth());
		int h = Math.min(sprite.contents().height(), image.getHeight());
		int imageX = Math.round(sprite.getU0() * atlasW), imageY = Math.round(sprite.getV0() * atlasH);
		int pad = Math.max(0, Math.min(imageX - sprite.getX(), imageY - sprite.getY()));
		for (int y = -pad; y < h + pad; y++) {
			int ty = imageY + y + yOffset;
			if (ty < 0 || ty >= this.height) {
				continue;
			}
			int sy = Math.clamp(y, 0, h - 1);
			for (int x = -pad; x < w + pad; x++) {
				int tx = imageX + x;
				if (tx < 0 || tx >= this.width) {
					continue;
				}
				int argb = image.getPixel(Math.clamp(x, 0, w - 1), sy);
				int o = (ty * this.width + tx) * 4;
				this.pixels.put(o, (byte) argb);
				this.pixels.put(o + 1, (byte) (argb >> 8));
				this.pixels.put(o + 2, (byte) (argb >> 16));
				this.pixels.put(o + 3, (byte) (argb >>> 24));
			}
		}
		Animation animation = animationOf(sprite.contents());
		if (animation != null) {
			animation.image = image;
			animation.x = imageX;
			animation.y = imageY + yOffset;
			animation.w = w;
			animation.h = h;
			animation.pad = pad;
			this.animations.add(animation);
		}
	}

	private static Animation animationOf(net.minecraft.client.renderer.texture.SpriteContents contents) {
		try {
			var field = net.minecraft.client.renderer.texture.SpriteContents.class.getDeclaredField("animatedTexture");
			field.setAccessible(true);
			Object animated = field.get(contents);
			if (animated == null) {
				return null;
			}
			Class<?> type = animated.getClass();
			var framesField = type.getDeclaredField("frames");
			var rowField = type.getDeclaredField("frameRowSize");
			var interpolateField = type.getDeclaredField("interpolateFrames");
			framesField.setAccessible(true);
			rowField.setAccessible(true);
			interpolateField.setAccessible(true);
			java.util.List<?> frames = (java.util.List<?>) framesField.get(animated);
			if (frames.size() < 2) {
				return null;
			}
			Animation a = new Animation();
			a.index = new int[frames.size()];
			a.time = new int[frames.size()];
			for (int k = 0; k < frames.size(); k++) {
				Object frame = frames.get(k);
				var indexField = frame.getClass().getDeclaredField("index");
				var timeField = frame.getClass().getDeclaredField("time");
				indexField.setAccessible(true);
				timeField.setAccessible(true);
				a.index[k] = indexField.getInt(frame);
				a.time[k] = Math.max(1, timeField.getInt(frame));
				a.cycle += a.time[k];
			}
			a.rowSize = Math.max(1, rowField.getInt(animated));
			a.interpolate = interpolateField.getBoolean(animated);
			return a;
		} catch (ReflectiveOperationException | RuntimeException e) {
			return null;
		}
	}

	public int animatedSprites() {
		return this.animations.size();
	}

	public record Region(int x, int y, int w, int h, ByteBuffer pixels) {}

	public void animate(long gameTime, java.util.function.Predicate<Region> sink) {
		for (Animation a : this.animations) {
			if (a.cycle <= 0) continue;
			int t = (int) (gameTime % a.cycle);
			int frameIdx = 0;
			int rem = t;
			for (int k = 0; k < a.time.length; k++) {
				if (rem < a.time[k]) {
					frameIdx = a.index[k];
					break;
				}
				rem -= a.time[k];
			}
			if (a.shown == frameIdx) continue;
			a.shown = frameIdx;
			int frameX = (frameIdx % a.rowSize) * a.w;
			int frameY = (frameIdx / a.rowSize) * a.h;
			int regionW = a.w + a.pad * 2;
			int regionH = a.h + a.pad * 2;
			ByteBuffer buf = ByteBuffer.allocateDirect(regionW * regionH * 4).order(ByteOrder.LITTLE_ENDIAN);
			for (int y = -a.pad; y < a.h + a.pad; y++) {
				int sy = frameY + Math.clamp(y, 0, a.h - 1);
				for (int x = -a.pad; x < a.w + a.pad; x++) {
					int sx = frameX + Math.clamp(x, 0, a.w - 1);
					int argb = a.image.getPixel(sx, sy);
					buf.put((byte) argb);
					buf.put((byte) (argb >> 8));
					buf.put((byte) (argb >> 16));
					buf.put((byte) (argb >>> 24));
				}
			}
			buf.flip();
			sink.test(new Region(a.x - a.pad, a.y - a.pad, regionW, regionH, buf));
		}
	}
}
