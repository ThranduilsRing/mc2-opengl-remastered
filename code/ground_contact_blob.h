//***************************************************************************
//
//	ground_contact_blob.h -- GROUND-CONTACT-BLOB-1
//
//	Per-mover contact-darkening disc, submitted into the existing TerrainDecal
//	batch (gos_PushDecal) each frame. Anchors mechs/vehicles visually to the
//	ground for close cinematic cameras (mc2_17-style cutscene) where the
//	screen-space shadow cascade alone reads as "actor pasted on a stage".
//
//	Gate: MC2_GROUND_CONTACT_BLOB (env var; default OFF).
//	Off behavior: renderGroundContactBlobs() is a no-op, touches no GL state.
//
//	Design (see .claude/TERRAIN-CINEMATIC-GROUNDING-1-RECON.md):
//	  - Movers only (mechs + vehicles), not static props.
//	  - Untextured solid tint quad (decal.frag Texcoord.x<-0.5 path) -- v0,
//	    zero new asset.
//	  - Texcoord.y<-0.5 additionally marks the quad as self-darkening so
//	    decal.frag SKIPS its inline sun-shadow multiply (avoids double
//	    darkening where a cast shadow already covers the blob).
//	  - Alpha is ambient-coupled (scaled by eye->ambient) and clamped so the
//	    blob never crushes to pure black on an already-dark map, and never
//	    washes out on a bright one.
//	  - Submitted BEFORE craterManager->render() so blobs land UNDER any
//	    footprint/crater/scorch decal sharing the same un-depth-sorted batch.
//
//	Copyright (C) Microsoft Corporation. All rights reserved.
//===========================================================================//

#ifndef GROUND_CONTACT_BLOB_H
#define GROUND_CONTACT_BLOB_H

// Iterates ObjectManager's live mechs + ground vehicles and pushes one
// untextured contact-darkening quad per mover into the TerrainDecal batch
// (gos_PushDecal). Call BEFORE craterManager->render() so blobs submit first
// (render UNDER footprints/craters in the shared un-depth-sorted batch).
//
// Gate-off (MC2_GROUND_CONTACT_BLOB unset/0): returns immediately, no-op.
void renderGroundContactBlobs(void);

#endif // GROUND_CONTACT_BLOB_H
