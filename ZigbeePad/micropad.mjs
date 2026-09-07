// zigbee2mqtt/external_converters/micropad.mjs

import exposes from 'zigbee-herdsman-converters/lib/exposes';

const e = exposes.presets;

const micropadAction = {
    cluster: 'genMultistateInput',
    type: ['attributeReport', 'readResponse'],

    convert: (model, msg, publish, options, meta) => {
        const value = Number(msg.data.presentValue);

        if (
            Number.isNaN(value) ||
            value === 0
        ) {
            return;
        }

        let action;

        // 1..9 = normal press
        if (value >= 1 && value <= 9) {
            action = `button_${value}_press`;
        }

        // 11..19 = double press
        else if (value >= 11 && value <= 19) {
            action = `button_${value - 10}_double`;
        }

        // 21..29 = hold
        else if (value >= 21 && value <= 29) {
            action = `button_${value - 20}_hold`;
        }

        else {
            return;
        }

        return {
            action,
        };
    },
};

export default {
    zigbeeModel: [
        'Micropad',
    ],

    model: 'Micropad',
    vendor: 'Monco',
    description: '3x3 Zigbee Micropad',

    fromZigbee: [
        micropadAction,
    ],

    toZigbee: [],

    exposes: [
        e.action([
            'button_1_press',
            'button_1_double',
            'button_1_hold',

            'button_2_press',
            'button_2_double',
            'button_2_hold',

            'button_3_press',
            'button_3_double',
            'button_3_hold',

            'button_4_press',
            'button_4_double',
            'button_4_hold',

            'button_5_press',
            'button_5_double',
            'button_5_hold',

            'button_6_press',
            'button_6_double',
            'button_6_hold',

            'button_7_press',
            'button_7_double',
            'button_7_hold',

            'button_8_press',
            'button_8_double',
            'button_8_hold',

            'button_9_press',
            'button_9_double',
            'button_9_hold',
        ]),
    ],
};