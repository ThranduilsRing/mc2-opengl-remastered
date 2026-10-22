vec3 evalShL2(vec3 n, vec3 sh[9]) {
    const float kSH_c1 = 0.429043;
    const float kSH_c2 = 0.511664;
    const float kSH_c3 = 0.743125;
    const float kSH_c4 = 0.886227;
    const float kSH_c5 = 0.247708;
    return
          kSH_c1 * sh[8] * (n.x * n.x - n.z * n.z)
        + kSH_c3 * sh[6] * (n.y * n.y)
        + kSH_c4 * sh[0]
        - kSH_c5 * sh[6]
        + 2.0 * kSH_c1 * sh[4] * (n.x * n.y)
        + 2.0 * kSH_c1 * sh[5] * (n.y * n.z)
        + 2.0 * kSH_c1 * sh[7] * (n.x * n.z)
        + 2.0 * kSH_c2 * sh[3] * n.x
        + 2.0 * kSH_c2 * sh[1] * n.y
        + 2.0 * kSH_c2 * sh[2] * n.z;
}
