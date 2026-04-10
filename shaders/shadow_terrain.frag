//#version 420 (version provided by prefix)

void main()
{
    // Explicit depth write — prevents AMD "empty shader" optimization
    gl_FragDepth = gl_FragCoord.z;
}
