// Deliberately invalid: the build must fail with a clear slangc diagnostic.
[shader("fragment")]
float4 main() : SV_Target0 { return undefined_symbol; }
