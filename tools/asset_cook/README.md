<!-- doc-status: current | reviewed:2026-10-22 -->
# Legacy asset manifest compatibility

Models now use `tools/content_pipeline/cook.py` and `docs/content-pipeline.md`.
`cook_all_stock.py` is a batch frontend for stock viewer meshdumps; it produces
one normal content package. Track G staging, texture cooking, assembly,
promotion and the duplicate GLB writer have been retired.

The old manifest validator and classification fixtures remain for reading
archived manifests. They do not cook or publish runtime assets.
