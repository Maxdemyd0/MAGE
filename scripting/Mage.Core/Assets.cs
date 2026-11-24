// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Maxdemyd0 and MAGE contributors
using Mage.Interop;

namespace Mage
{
    public enum ModelPhysics { None = 0, Static = 1, Dynamic = 2 }
    public enum TextureSlot { BaseColor = 0, MetallicRoughness = 1, Normal = 2, Emissive = 3 }

    /// <summary>Imported files. Paths are relative to the asset folder (or absolute).</summary>
    public static class Assets
    {
        /// <summary>Spawns a .gltf/.glb/.fbx/.obj model. Returns <see cref="Physics.NoEntity"/> if it cannot be loaded.</summary>
        public static uint SpawnModel(string path, Vector3 position, Quaternion rotation, Vector3 scale, ModelPhysics physics = ModelPhysics.None) =>
            InternalCalls.SpawnModel(path, position, rotation, scale, (int)physics);

        /// <summary>Sets a texture on a primitive entity; an empty path clears it.</summary>
        public static bool SetTexture(uint entity, string path, TextureSlot slot = TextureSlot.BaseColor) =>
            InternalCalls.SetEntityTexture(entity, (int)slot, path);
    }

    /// <summary>Sound playback (.wav .mp3 .flac .ogg).</summary>
    public static class Audio
    {
        /// <param name="position">World position for 3D sound, or null for flat playback.</param>
        /// <returns>Sound handle, 0 on failure.</returns>
        public static uint Play(string path, float volume = 1f, float pitch = 1f, bool loop = false, Vector3? position = null) =>
            InternalCalls.PlayAudio(path, volume, pitch, loop, position);

        public static void Stop(uint sound) => InternalCalls.StopAudio(sound);
    }

    /// <summary>On-screen text. Call Draw every frame from OnUpdate.</summary>
    public static class Text
    {
        /// <summary>Loads a .ttf/.otf font. Returns 0 on failure.</summary>
        public static int LoadFont(string path, float size = 24f) => InternalCalls.LoadFont(path, size);

        /// <param name="rgba">Colour as 0xRRGGBBAA.</param>
        public static void Draw(string text, float x, float y, float size = 24f, uint rgba = 0xFFFFFFFF, int font = 0) =>
            InternalCalls.DrawLabel(text, x, y, size, rgba, font);
    }
}
