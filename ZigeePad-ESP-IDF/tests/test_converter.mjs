import assert from 'node:assert/strict';
import {readFile} from 'node:fs/promises';

// Test the converter itself; mock only the UI exposes dependency.
const source = await readFile(new URL('../zigbee2mqtt/micropad.mjs', import.meta.url), 'utf8');
const testSource = source.replace(
    "import * as exposes from 'zigbee-herdsman-converters/lib/exposes';",
    'const exposes = {presets: {action: (values) => values}};',
);
assert.notEqual(source, testSource);
const {default: definition} = await import(`data:text/javascript;base64,${Buffer.from(testSource).toString('base64')}`);
assert.deepEqual(definition.zigbeeModel, ['Micropad']);
assert.equal(definition.vendor, 'Monco');
const converter = definition.fromZigbee[0];
assert.equal(converter.cluster, 'genMultistateInput');
assert.deepEqual(converter.type, ['attributeReport']);
assert.equal(definition.exposes[0].length, 27);
for (let key = 1; key <= 9; key++) {
    for (const [offset, suffix] of [[0, 'press'], [10, 'double'], [20, 'hold']]) {
        const expected = {action: `button_${key}_${suffix}`};
        const message = {data: {presentValue: key + offset}};
        assert.deepEqual(converter.convert(null, message), expected);
        assert.deepEqual(converter.convert(null, message), expected); // consecutive identical presses
        assert.ok(definition.exposes[0].includes(expected.action));
    }
}
for (const value of [undefined, null, NaN, Infinity, -1, 0, 10, 20, 30, 1.5, 'bad']) {
    assert.equal(converter.convert(null, {data: {presentValue: value}}), undefined);
}
console.log('Converter OK: 27 actions, repeated events, invalid values, report-only input.');
