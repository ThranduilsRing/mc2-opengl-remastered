// PROP-ARRAY-BUCKETS (GameOS/gameos/prop_array_buckets.h): the static-prop alpha-ON albedo group is up to
// PROP_ALPHA_ON_BUCKETS texture arrays split by size; a packet's alpha-ON layer is (bucket << 16) | layer-in-bucket.
// The bucket is constant per draw; it indexes a sampler array, so sample with nonuniformEXT(bucket).
#ifndef PROP_ARRAY_BUCKET_GLSL
#define PROP_ARRAY_BUCKET_GLSL
#define PROP_ALPHA_ON_BUCKETS 4
#define PROP_BUCKET_LAYER_BITS 16
int propBucketOf(int layer) { return layer >> PROP_BUCKET_LAYER_BITS; }
int propBucketLayer(int layer) { return layer & ((1 << PROP_BUCKET_LAYER_BITS) - 1); }
#endif
