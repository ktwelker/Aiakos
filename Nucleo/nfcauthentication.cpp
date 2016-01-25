#include "nfcauthentication.h"
#include "RHCRC.h"

extern NfcAdapter nfc;
extern NtagSramAdapter ntagAdapter;
void print(const byte* array, byte length);
void print(nfcAuthentication::ss state, byte data);


static int RNG(uint8_t *dest, unsigned size);
byte writePin;
byte readPin;
extern uint32_t ulStartTime;
extern uint32_t ulStartTime2;


nfcAuthentication::nfcAuthentication(NtagSramAdapter* nfca):
    _ntagAdapter(nfca)
{
    _bIsDongle = true;
    cryptop.setInitiator(_bIsDongle);
    writePin =  3;
    readPin =  4;
    pinMode(7, OUTPUT);//debug
}

nfcAuthentication::nfcAuthentication(NfcAdapter* nfca):
    _nfcAdapter(nfca)
{
    _bIsDongle = false;
    cryptop.setInitiator(_bIsDongle);
    writePin =  18;
    readPin =  19;
}

void nfcAuthentication::begin()
{
    pinMode(writePin, OUTPUT);
    pinMode(readPin, OUTPUT);
    if (_bIsDongle)
    {
        _ntagAdapter->begin();
        byte data[_ntagAdapter->getUidLength()];
        _ntagAdapter->getUid(data, _ntagAdapter->getUidLength());
        cryptop.setNFCIDi(data, _ntagAdapter->getUidLength());
    } else
    {
        _nfcAdapter->begin();
    }
    cryptop.generateAsymmetricKey(&RNG);
    _backoffTime=millis();
}

bool nfcAuthentication::loop()
{
    if (_state == WAITING_FOR_PEER)
    {
        _pairingStartTime = millis();
    }
    if (millis() > _pairingStartTime + PAIRING_TIMEOUT)
    {
        _state = WAITING_FOR_PEER;
        _pairingStartTime = millis();
    }
    if (_bIsDongle)
    {
        digitalWrite(7, _ntagAdapter->rfBusy() ? HIGH : LOW);
        return tagLoop();
    } else
    {
        return readerLoop();
    }
}

bool nfcAuthentication::readerLoop()
{
    byte payload[64];

    if (millis() < _backoffTime + 150)
    {
        //Avoid reading tag too frequently which may inhibit I²C access to the tag
        return false;
    }
    if (!_nfcAdapter->tagPresent()) {
        _state = WAITING_FOR_PEER;
        return false;
    }
    switch (_state)
    {
    case WAITING_FOR_PEER:
        if (_nfcAdapter->tagPresent()) {
            _state = READING_PUBLIC_KEY;
        }
        break;
    case READING_PUBLIC_KEY:
        if (getTagData())
        {
            payload[0] = NfcSec01::QB;
            cryptop.getPublicKey(payload + 1);
            if (write(payload, 1 + cryptop.getPublicKeySize()))
            {
                Serial.println("QB written");
                _state = READING_NONCE;
            }
        }
        break;
    case READING_NONCE:
        if (getTagData())
        {
            payload[0] = NfcSec01::NB;
            cryptop.getLocalNonce(payload + 1);
            if (write(payload, 1 + cryptop.getNonceSize()))
            {
                Serial.println("NB written");
                _state = WAITING_FOR_MAC_TAG;
            }
        }
        break;
    case WAITING_FOR_MAC_TAG:
        if (getTagData())
        {
            payload[0] = NfcSec01::MAC_TAG_B;
            cryptop.generateKeyConfirmationTag(payload + 1);
            if (write(payload, 1 + cryptop.getMacTagSize()))
            {
                Serial.println("MacTagB written");
                _state = WAITING_FOR_PEER;
                return true;
            }
        }
        break;
    default:
        _state = WAITING_FOR_PEER;
        break;
    }
    _backoffTime=millis();
    return false;
}


bool nfcAuthentication::longRfBusyPulseFound()
{
    static byte state=0;
    static unsigned long pulseStart;

    switch(state)
    {
    case 0:
        if(_ntagAdapter->rfBusy())
        {
            state=1;
            pulseStart=millis();
        }
        break;
    case 1:
        if(!_ntagAdapter->rfBusy())
        {
            //end of pulse detected
            if(millis()>pulseStart+50)
            {
                //long pulse detected
                state=0;
                return true;
            }else
            {
                //too short pulse detected
                state=0;
            }
        }
        break;
    default:
        state=0;
        break;
    }
    return false;
}

bool nfcAuthentication::tagLoop() {
    byte payload[64];

    if(!longRfBusyPulseFound())
    {
        return false;
    }
    if ((!_ntagAdapter->readerPresent()))
    {
        _state = WAITING_FOR_PEER;
    }
    switch (_state)
    {
    case WAITING_FOR_PEER:
        if (_ntagAdapter->readerPresent())
        {
            payload[0] = NfcSec01::QA;
            cryptop.getPublicKey(payload + 1);
            if (write(payload, 1 + cryptop.getPublicKeySize()))
            {
                Serial.println("QA written");
                _state = READING_PUBLIC_KEY;
            }
        }
        break;
    case READING_PUBLIC_KEY:
        if (getTagData())
        {
            payload[0] = NfcSec01::NA;
            cryptop.getLocalNonce(payload + 1);
            if (write(payload, 1 + cryptop.getNonceSize()))
            {
                Serial.println("NA written");
                _state = READING_NONCE;
            }
        }
        break;
    case READING_NONCE:
        if (getTagData())
        {
            payload[0] = NfcSec01::MAC_TAG_A;
            cryptop.generateKeyConfirmationTag(payload + 1);
            if (write(payload, 1 + cryptop.getMacTagSize()))
            {
                Serial.println("MacTagA written");
                _state = WAITING_FOR_MAC_TAG;
            }
        }
        break;
    case WAITING_FOR_MAC_TAG:
        if (getTagData())
        {
            _state = WAITING_FOR_PEER;
            return true;
        }
        break;
    default:
        _state = WAITING_FOR_PEER;
        break;
    }
    _backoffTime=millis();
    return false;
}


bool nfcAuthentication::getTagData()
{
    //Check if tag has data
    if (!read())
    {
        return false;
    }
    print(_state, _payload[0]);
    switch (_state) {
    case READING_PUBLIC_KEY:
        if (!_bIsDongle)
        {
            if (_payload[0] != NfcSec01::QA)
            {
                return false;
            }
        }
        else
        {
            if (_payload[0] == NfcSec01::QA)
            {
                return false;
            }
            if (_payload[0] != NfcSec01::QB)
            {
                _state = WAITING_FOR_PEER;
                return false;
            }
        }
        //Package contents:
        //  MSG_ID | QA or MSG_ID | QB
        cryptop.setRemotePublicKey(_payload + 1);
        cryptop.generateRandomNonce(&RNG);
        return true;
    case READING_NONCE:
        if (!_bIsDongle)
        {
            if (_payload[0] == NfcSec01::QB)
            {
                return false;
            }
            if (_payload[0] != NfcSec01::NA)
            {
                _state = READING_PUBLIC_KEY;
                return false;
            }
        }
        else
        {
            if (_payload[0] == NfcSec01::NA)
            {
                return false;
            }
            if (_payload[0] == NfcSec01::QB)
            {
                _state = READING_PUBLIC_KEY;
                return false;
            }
            if (_payload[0] != NfcSec01::NB)
            {
                _state = WAITING_FOR_PEER;
                return false;
            }
        }
        //Package contents:
        //  MSG_ID | NA or MSG_ID | NB
        //Reader and tag use the same NFCID because the reader doesn't have an NFCID.
        cryptop.setNFCIDi(_remoteId, _remoteIdLength);
        return cryptop.calcMasterKeySSE(_payload + 1, _remoteId, _remoteIdLength);
        break;
    case WAITING_FOR_MAC_TAG:
        if (!_bIsDongle)
        {
            if (_payload[0] == NfcSec01::NB)
            {
                return false;
            }
            if (_payload[0] == NfcSec01::NA)
            {
                _state = READING_NONCE;
                return false;
            }
            if (_payload[0] != NfcSec01::MAC_TAG_A)
            {
                _state = READING_PUBLIC_KEY;
                return false;
            }
        }
        else
        {
            if (_payload[0] == NfcSec01::MAC_TAG_A)
            {
                return false;
            }
            if (_payload[0] == NfcSec01::NB)
            {
                _state = READING_NONCE;
                return false;
            }
            if (_payload[0] != NfcSec01::MAC_TAG_B)
            {
                _state = WAITING_FOR_PEER;
                return false;
            }
        }
        //Package contents:
        // MSG_ID | MAC_TAG_A or MSG_ID | MAC_TAG_B
        return cryptop.checkKeyConfirmation(_payload + 1);
    default:
        return false;
    }
    return false;
}

bool nfcAuthentication::read()
{
    uint16_t crc = 0xffff;
    digitalWrite(readPin, HIGH);
    if (!_bIsDongle && !_nfcAdapter->tagPresent())
    {
        return false;
    }
    NfcTag nf = _bIsDongle ? _ntagAdapter->read(200) : _nfcAdapter->read();//takes 101ms on Arduino Due
    digitalWrite(readPin, LOW);
    _remoteIdLength = nf.getUidLength();
    nf.getUid(_remoteId, _remoteIdLength);
    if (!nf.hasNdefMessage()) {
        return false;
    }
    NdefMessage nfm = nf.getNdefMessage();
    if (nfm.getRecordCount() == 0)
    {
        return false;
    }
    NdefRecord ndf = nfm.getRecord(0);
    _payloadLength = ndf.getPayloadLength();
    ndf.getPayload(_payload);
    for (byte i = 0; i < _payloadLength - 2; i++)
    {
        crc = RHcrc_ccitt_update(crc, _payload[i]);
    }
    if (lowByte(crc) != _payload[_payloadLength - 2] || highByte(crc) != _payload[_payloadLength - 1])
    {
        Serial.println("Wrong CRC");
        return false;
    }
    _payloadLength -= 2;
    return true;
}

bool nfcAuthentication::write(byte* data, byte length)
{
    bool bResult;
    NdefMessage message = NdefMessage();
    uint16_t crc = 0xffff;
    for (byte i = 0; i < length; i++)
    {
        crc = RHcrc_ccitt_update(crc, data[i]);
    }
    data[length] = lowByte(crc);
    data[length + 1] = highByte(crc);
    message.addUnknownRecord(data, length + 2);
    digitalWrite(writePin, HIGH);
    if (_bIsDongle)
    {
        bResult = _ntagAdapter->write(message, 200);
    }
    else
    {
        bResult = _nfcAdapter->write(message);
    }
    digitalWrite(writePin, LOW);
    return bResult;
}

//TODO: replace by safe external RNG
static int RNG(uint8_t *dest, unsigned size) {
    // Use the least-significant bits from the ADC for an unconnected pin (or connected to a source of
    // random noise). This can take a long time to generate random data if the result of analogRead(0)
    // doesn't change very frequently.
    while (size) {
        uint8_t val = 0;
        for (unsigned i = 0; i < 8; ++i) {
            int init = analogRead(0);
            int count = 0;
            while (analogRead(0) == init) {
                ++count;
            }

            if (count == 0) {
                val = (val << 1) | (init & 0x01);
            } else {
                val = (val << 1) | (count & 0x01);
            }
        }
        *dest = val;
        ++dest;
        --size;
    }
    // NOTE: it would be a good idea to hash the resulting random data using SHA-256 or similar.
    return 1;
}

void print(nfcAuthentication::ss state, byte data)
{
    switch (state) {
    case nfcAuthentication::WAITING_FOR_PEER:
        Serial.print("WAITING_FOR_PEER");
        break;
    case nfcAuthentication::READING_PUBLIC_KEY:
        Serial.print("READING_PUBLIC_KEY");
        break;
    case nfcAuthentication::READING_NONCE:
        Serial.print("READING_NONCE");
        break;
    case nfcAuthentication::WAITING_FOR_MAC_TAG:
        Serial.print("WAITING_FOR_MAC_TAG");
        break;
    default:
        Serial.print("unknown state");
        break;
    }
    Serial.print("\t");
    switch (data)
    {
    case NfcSec01::QA:
        Serial.print("QA");
        break;
    case NfcSec01::QB:
        Serial.print("QB");
        break;
    case NfcSec01::NA:
        Serial.print("NA");
        break;
    case NfcSec01::NB:
        Serial.print("NB");
        break;
    case NfcSec01::MAC_TAG_A:
        Serial.print("MAC_TAG_A");
        break;
    case NfcSec01::MAC_TAG_B:
        Serial.print("MAC_TAG_B");
        break;
    default:
        Serial.print("unknown data");
        break;
    }
    Serial.println();
}

void print(const byte* array, byte length)
{
    for (byte i = 0; i < length; i++)
    {
        Serial.print(array[i], HEX);
        Serial.print(" ");
        if ((i + 1) % 16 == 0)
        {
            Serial.println();
        }
    }
    Serial.println();
}
