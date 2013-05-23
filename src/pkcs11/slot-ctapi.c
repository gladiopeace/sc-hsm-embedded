/*
 *  ---------
 * |.**> <**.|  CardContact Software & System Consulting
 * |*       *|  32429 Minden, Germany (www.cardcontact.de)
 * |*       *|  Copyright (c) 1999-2003. All rights reserved
 * |'**> <**'|  See file COPYING for details on licensing
 *  --------- 
 *
 * The Smart Card Development Platform (SCDP) provides a basic framework to
 * implement smartcard aware applications.
 *
 * Abstract :       Functions for token management in a specific slot
 *
 * Author :         Frank Thater (FTH)
 *
 *****************************************************************************/

/**
 * \file    slot.c
 * \author  Frank Thater (fth)
 * \brief   Functions for token management in a specific slot
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>

#include <pkcs11/slot.h>
#include <pkcs11/token.h>
#include <pkcs11/slotpool.h>

#include <strbpcpy.h>

#include <ctccid/ctapi.h>

extern struct p11Context_t *context;

/**
 * addToken adds a token to the specified slot.
 *
 * @param slot       Pointer to slot structure.
 * @param token      Pointer to token structure.
 *
 * @return          
 *                   <P><TABLE>
 *                   <TR><TD>Code</TD><TD>Meaning</TD></TR>
 *                   <TR>
 *                   <TD>CKR_OK                                 </TD>
 *                   <TD>Success                                </TD>
 *                   </TR>
 *                   <TR>
 *                   <TD>CKR_FUNCTION_FAILED                    </TD>
 *                   <TD>There is already a token in the slot   </TD>
 *                   </TR>
 *                   </TABLE></P>
 */
static int addToken(struct p11Slot_t *slot, struct p11Token_t *token)
{
	if (slot->token != NULL) {
		return CKR_FUNCTION_FAILED;
	}

	slot->token = token;                     /* Add token to slot                */
	slot->info.flags |= CKF_TOKEN_PRESENT;   /* indicate the presence of a token */

	return CKR_OK;
}



/**
 * removeToken removes a token from the specified slot.
 *
 * @param slot       Pointer to slot structure.
 *
 * @return          
 *                   <P><TABLE>
 *                   <TR><TD>Code</TD><TD>Meaning</TD></TR>
 *                   <TR>
 *                   <TD>CKR_OK                                 </TD>
 *                   <TD>Success                                </TD>
 *                   </TR>
 *                   <TR>
 *                   <TD>CKR_FUNCTION_FAILED                    </TD>
 *                   <TD>There is no token in the slot          </TD>
 *                   </TR>
 *                   </TABLE></P>
 */
static int removeToken(struct p11Slot_t *slot)
{
	if (slot->token == NULL) {
		return CKR_FUNCTION_FAILED;
	}

	slot->info.flags &= ~CKF_TOKEN_PRESENT;
	freeToken(slot);

	return CKR_TOKEN_NOT_PRESENT;
}



/*
 *  Process an ISO 7816 APDU with the underlying terminal hardware.
 *
 *  CLA     : Class byte of instruction
 *  INS     : Instruction byte
 *  P1      : Parameter P1
 *  P2      : Parameter P2
 *  OutLen  : Length of outgoing data (Lc)
 *  OutData : Outgoing data or NULL if none
 *  InLen   : Length of incoming data (Le)
 *  InData  : Input buffer for incoming data
 *  InSize  : buffer size
 *  SW1SW2  : Address of short integer to receive SW1SW2
 *
 *  Returns : < 0 Error > 0 Bytes read
 */
static int transmitAPDUwithCTAPI(struct p11Slot_t *slot, int todad,
		unsigned char CLA, unsigned char INS, unsigned char P1, unsigned char P2,
		int OutLen, unsigned char *OutData,
		int InLen, unsigned char *InData, int InSize, unsigned short *SW1SW2)
{
	int  rv, rc, r, retry;
	unsigned short lenr;
	unsigned char dad, sad;
	unsigned char scr[MAX_APDULEN], *po;

	FUNC_CALLED();

	retry = 2;

	while (retry--) {
		scr[0] = CLA;
		scr[1] = INS;
		scr[2] = P1;
		scr[3] = P2;
		po = scr + 4;
		rv = 0;

		if (OutData && OutLen) {
			if ((OutLen <= 255) && (InLen <= 255)) {
				*po++ = (unsigned char)OutLen;
			} else {
				*po++ = 0;
				*po++ = (unsigned char)(OutLen >> 8);
				*po++ = (unsigned char)(OutLen & 0xFF);
			}
			memcpy(po, OutData, OutLen);
			po += OutLen;
		}

		if (InData && InSize) {
			if ((InLen <= 255) && (OutLen <= 255)) {
				*po++ = (unsigned char)InLen;
			} else {
				if (InLen >= 65556)
					InLen = 0;

				if (!OutData) {
					*po++ = 0;
				}
				*po++ = (unsigned char)(InLen >> 8);
				*po++ = (unsigned char)(InLen & 0xFF);
			}
		}

		sad  = HOST;
		dad  = todad;
		lenr = sizeof(scr);

		rc = CT_data((unsigned short)slot->id, &dad, &sad, po - scr, scr, &lenr, scr);

		if (rc < 0)
			FUNC_FAILS(rc, "CT_data failed");

		if (scr[lenr - 2] == 0x6C) {
			InLen = scr[lenr - 1];
			continue;
		}

		rv = lenr - 2;

		if (rv > InSize)
			rv = InSize;

		if (InData)
			memcpy(InData, scr, rv);

		if ((scr[lenr - 2] == 0x9F) || (scr[lenr - 2] == 0x61))
			if (InData && InSize) {             /* Get Response             */
				r = transmitAPDU(slot,
						(unsigned char)((CLA == 0xE0) || (CLA == 0x80) ?
								0x00 : CLA), 0xC0, 0, 0,
								0, NULL,
								scr[1], InData + rv, InSize - rv, SW1SW2);

				if (r < 0)
					FUNC_FAILS(rc, "GET RESPONSE failed");

				rv += r;
			} else
				*SW1SW2 = 0x9000;
		else
			*SW1SW2 = (scr[lenr - 2] << 8) + scr[lenr - 1];
		break;
	}

	FUNC_RETURNS(rv);
}



/*
 *  Process an ISO 7816 APDU with the underlying terminal hardware.
 *
 *  CLA     : Class byte of instruction
 *  INS     : Instruction byte
 *  P1      : Parameter P1
 *  P2      : Parameter P2
 *  OutLen  : Length of outgoing data (Lc)
 *  OutData : Outgoing data or NULL if none
 *  InLen   : Length of incoming data (Le)
 *  InData  : Input buffer for incoming data
 *  InSize  : buffer size
 *  SW1SW2  : Address of short integer to receive SW1SW2
 *
 *  Returns : < 0 Error > 0 Bytes read
 */
int transmitAPDU(struct p11Slot_t *slot,
		unsigned char CLA, unsigned char INS, unsigned char P1, unsigned char P2,
		int OutLen, unsigned char *OutData,
		int InLen, unsigned char *InData, int InSize, unsigned short *SW1SW2)

{
	int rc;
#ifdef DEBUG
	char scr[4196], *po;

	sprintf(scr, "C-APDU: %02X %02X %02X %02X ", CLA, INS, P1, P2);
	po = strchr(scr, '\0');

	if (OutLen && OutData) {
		sprintf(po, "Lc=%02X(%d) ", OutLen, OutLen);
		po = strchr(scr, '\0');
		if (OutLen > 2048) {
			decodeBCDString(OutData, 2048, po);
			strcat(po, "..");
		} else {
			decodeBCDString(OutData, OutLen, po);
		}
		po = strchr(scr, '\0');
		strcpy(po, " ");
		po++;
	}

	if (InData && InSize)
		sprintf(po, "Le=%02X(%d)", InLen, InLen);

	debug("%s\n", scr);
#endif

	rc = transmitAPDUwithCTAPI(slot, 0, CLA, INS, P1, P2,
			OutLen, OutData,
			InLen, InData, InSize, SW1SW2);
#ifdef DEBUG
if (rc > 0) {
	sprintf(scr, "R-APDU: Lr=%02X(%d) ", rc, rc);
	po = strchr(scr, '\0');
	if (rc > 2048) {
		decodeBCDString(InData, 2048, po);
		strcat(scr, "..");
	} else {
		decodeBCDString(InData, rc, po);
	}

	po = strchr(scr, '\0');
	sprintf(po, " SW1/SW2=%04X", *SW1SW2);
} else
	sprintf(scr, "R-APDU: rc=%d SW1/SW2=%04X", rc, *SW1SW2);

debug("%s\n", scr);
#endif
return rc;
}



/**
 * checkForNewToken looks into a specific slot for a token.
 *
 * @param slot       Pointer to slot structure.
 *
 * @return          
 *                   <P><TABLE>
 *                   <TR><TD>Code</TD><TD>Meaning</TD></TR>
 *                   <TR>
 *                   <TD>CKR_OK                                 </TD>
 *                   <TD>Success                                </TD>
 *                   </TR>
 *                   <TR>
 *                   <TD>CKR_HOST_MEMORY                        </TD>
 *                   <TD>Error getting memory (malloc)          </TD>
 *                   </TR>
 *                   <TR>
 *                   <TD>CKR_GENERAL_ERROR                      </TD>
 *                   <TD>Error opening slot directory           </TD>
 *                   </TR>
 *                   </TABLE></P>
 */
static int checkForNewToken(struct p11Slot_t *slot)
{
	struct p11Token_t *ptoken;
	unsigned char rsp[260];
	unsigned short lr;
	unsigned char dad, sad;
	char scr[_MAX_PATH];
	int fh, rc, i;
	unsigned short SW1SW2;

	FUNC_CALLED();

	if (slot->closed) {
		FUNC_RETURNS(CKR_TOKEN_NOT_PRESENT);
	}

	rc = transmitAPDUwithCTAPI(slot, 1, 0x20, 0x13, 0x01, 0x80, 0, NULL, 0, rsp, sizeof(rsp), &SW1SW2);

	if (rc == ERR_CT) {
		closeSlot(slot);
	}

	if (rc < 0) {
		FUNC_FAILS(CKR_DEVICE_ERROR, "GET_STATUS failed");
	}

	if ((SW1SW2 != 0x9000) || (rc < 3) || (rsp[0] != 0x80) || (rsp[1] == 0) || (rsp[1] > rc - 2)) {
		FUNC_FAILS(CKR_DEVICE_ERROR, "GET_STATUS returned invalid response");
	}

	if (!(rsp[2] & 0x01)) {	// No Card in reader
		FUNC_RETURNS(CKR_TOKEN_NOT_PRESENT);
	}

	rc = transmitAPDUwithCTAPI(slot, 1, 0x20, 0x12, 0x01, 0x01, 0, NULL, 0, rsp, sizeof(rsp), &SW1SW2);

	if (rc < 0) {
		FUNC_FAILS(CKR_TOKEN_NOT_RECOGNIZED, "REQUEST ICC failed");
	}

	if (SW1SW2 != 0x9001) {
		FUNC_FAILS(CKR_TOKEN_NOT_RECOGNIZED, "Reset failed");
	}

	rc = newToken(slot, &ptoken);

	if (rc != CKR_OK) {
		FUNC_FAILS(rc, "newSmartCardHSMToken failed()");
	}

	rc = addToken(slot, ptoken);

	FUNC_RETURNS(rc);
}



/**
 * checkForRemovedToken looks into a specific slot for a removed token.
 *
 * @param slot       Pointer to slot structure.
 *
 * @return
 *                   <P><TABLE>
 *                   <TR><TD>Code</TD><TD>Meaning</TD></TR>
 *                   <TR>
 *                   <TD>CKR_OK                                 </TD>
 *                   <TD>Success                                </TD>
 *                   </TR>
 *                   <TR>
 *                   <TD>CKR_HOST_MEMORY                        </TD>
 *                   <TD>Error getting memory (malloc)          </TD>
 *                   </TR>
 *                   <TR>
 *                   <TD>CKR_GENERAL_ERROR                      </TD>
 *                   <TD>Error opening slot directory           </TD>
 *                   </TR>
 *                   </TABLE></P>
 */
static int checkForRemovedToken(struct p11Slot_t *slot)
{
	struct p11Token_t *ptoken;
	unsigned char rsp[260];
	unsigned short lr;
	unsigned char dad, sad;
	char scr[_MAX_PATH];
	int fh, rc, i;
	unsigned short SW1SW2;

	FUNC_CALLED();

	rc = transmitAPDUwithCTAPI(slot, 1, 0x20, 0x13, 0x01, 0x80, 0, NULL, 0, rsp, sizeof(rsp), &SW1SW2);

	if (rc == ERR_CT) {					// Reader or USB-Device removed
		removeToken(slot);
		closeSlot(slot);
	}

	if (rc < 0) {
		FUNC_FAILS(CKR_GENERAL_ERROR, "GET_STATUS failed");
	}

	if ((SW1SW2 != 0x9000) || (rc < 3) || (rsp[0] != 0x80) || (rsp[1] == 0) || (rsp[1] > rc - 2)) {
		FUNC_FAILS(CKR_GENERAL_ERROR, "GET_STATUS returned invalid response");
	}

	if (rsp[2] & 0x01) {	// Token still in reader
		FUNC_RETURNS(CKR_OK);
	}

	rc = removeToken(slot);

	FUNC_RETURNS(rc);
}



int getToken(struct p11Slot_t *slot, struct p11Token_t **token)
{
	int rc;
	FUNC_CALLED();

	if (slot->token) {
		rc = checkForRemovedToken(slot);
	} else {
		rc = checkForNewToken(slot);
	}

	*token = slot->token;
	return rc;
}



int findSlotObject(struct p11Slot_t *slot, CK_OBJECT_HANDLE handle, struct p11Object_t **object, int publicObject)
{
	int rc;
	struct p11Token_t *token;

	rc = getToken(slot, &token);
	if (rc != CKR_OK) {
		return rc;
	}

	rc = findObject(token, handle, object, publicObject);

	return rc < 0 ? CKR_GENERAL_ERROR : CKR_OK;
}



int updateSlots(struct p11SlotPool_t *pool)
{
	struct p11Slot_t *slot;
	unsigned short ctn;
	char scr[20];
	int rc;

	FUNC_CALLED();

	slot = pool->list;
	while (slot) {
		if (slot->closed) {
			ctn = (unsigned short)slot->id;
			rc = CT_init(ctn, ctn - 1);

			if (rc != OK) {
#ifdef DEBUG
				debug("CT_init returns %d\n", rc);
#endif
			} else {
				slot->closed = FALSE;
			}
		}
		slot = slot->next;
	}

	while (pool->numberOfSlots < MAX_SLOTS) {
		ctn = (unsigned short)pool->nextSlotID;

		rc = CT_init(ctn, ctn - 1);

		if (rc != OK) {
#ifdef DEBUG
			debug("CT_init returns %d\n", rc);
#endif
			break;
		}

		slot = (struct p11Slot_t *) malloc(sizeof(struct p11Slot_t));

		if (slot == NULL) {
			FUNC_FAILS(CKR_HOST_MEMORY, "Out of memory");
		}

		memset(slot, 0x00, sizeof(struct p11Slot_t));

		sprintf(scr, "CT-API Port #%d", ctn - 1);
		strbpcpy(slot->info.slotDescription,
				scr,
				sizeof(slot->info.slotDescription));

		strbpcpy(slot->info.manufacturerID,
				"CardContact",
				sizeof(slot->info.manufacturerID));

		slot->info.hardwareVersion.minor = 0;
		slot->info.hardwareVersion.major = 0;

		slot->info.firmwareVersion.minor = 0;
		slot->info.firmwareVersion.major = 0;

		slot->info.flags = CKF_REMOVABLE_DEVICE | CKF_HW_SLOT;
		addSlot(context->slotPool, slot);
	}

	FUNC_RETURNS(CKR_OK);
}



int closeSlot(struct p11Slot_t *slot)
{
	int rc;

	FUNC_CALLED();

	rc = CT_close((unsigned short)slot->id);

	if (rc != OK) {
#ifdef DEBUG
		debug("CT_close returns %d\n", rc);
#endif
	}

	slot->closed = TRUE;

	FUNC_RETURNS(CKR_OK);
}