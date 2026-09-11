import * as exposes from 'zigbee-herdsman-converters/lib/exposes';

const actions = Array.from({length: 9}, (_, i) =>
    ['press', 'double', 'hold'].map((kind) => `button_${i + 1}_${kind}`)).flat();

const micropadAction = {
    cluster: 'genMultistateInput',
    // A read is device state, not a new physical press. Avoid replay on interview.
    type: ['attributeReport'],
    convert: (model, msg) => {
        const value = Number(msg.data.presentValue);
        if (!Number.isInteger(value)) return;
        if (value >= 1 && value <= 9) return {action: `button_${value}_press`};
        if (value >= 11 && value <= 19) return {action: `button_${value - 10}_double`};
        if (value >= 21 && value <= 29) return {action: `button_${value - 20}_hold`};
    },
};

export default {
    zigbeeModel: ['Micropad'],
    model: 'Micropad',
    vendor: 'Monco',
    description: '3x3 Zigbee Micropad',
    fromZigbee: [micropadAction],
    toZigbee: [],
    exposes: [exposes.presets.action(actions)],
};
