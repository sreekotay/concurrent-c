/* Shared by Track A drivers — inject or eval as a string. */
function makePayload(i) {
    const payload = new Uint8Array(64 * 1024);
    payload[0] = i & 255;

    const a = { i: i, payload: payload };
    const b = { parent: a };
    a.child = b;

    return function () {
        if (a.i !== i)
            throw new Error("corrupt");
        return a.payload[0];
    };
}

function makePayloadBatch(n) {
    const out = [];
    for (let i = 0; i < n; i++)
        out.push(makePayload(i));
    return out;
}
