
#include "MDB.h"
#include <SPI.h>
#include <MFRC522.h>
#include <SoftwareSerial.h>
// SoftwareSerial for debugging
#define SOFTWARESERIAL_DEBUG_ENABLED 1

#ifdef SOFTWARESERIAL_DEBUG_ENABLED
#define RX_DEBUG_PIN    2
#define TX_DEBUG_PIN    3
#endif

// SPI Slaves Settings
#define RC522_SLS_PIN   10     // MFRC522 SlaveSelect Pin
#define RC522_RST_PIN   9     // SPI RESET Pin


typedef enum {GET, POST, HEAD} HTTP_Method;    // 'keys' for HTTP functions

#ifdef SOFTWARESERIAL_DEBUG_ENABLED
SoftwareSerial Debug(RX_DEBUG_PIN, TX_DEBUG_PIN);
#endif

// SIMCOM module instance
//SoftwareSerial SIM900(SIMCOM_RX_PIN, SIMCOM_TX_PIN);
// Create MFRC522 instance
MFRC522 mfrc522(RC522_SLS_PIN, RC522_RST_PIN);
// String object for containing HEX represented UID
String uid_str_obj = "";
// Store the time of CSH_S_ENABLED start
uint32_t global_time = 0;
// Static Server port and address
String server_url_str = "http://xxx.xxx.xxx.xxx:xxx/api/Vending/";
//uint8_t in_progress = 0;

void setup() {
  uint16_t test = 0;
    // For debug LEDs
    DDRC |= 0b00111111; // PORTC LEDs for 6 commands in MDB_CommandHandler() switch-case
    //PORTC |= 0b00111111; 
#ifdef SOFTWARESERIAL_DEBUG_ENABLED
    Debug.begin(9600);
    Debug.println(F("Debug mode enabled"));
#endif
    // == Init card reader START ==
    SPI.begin();        // Init SPI bus, MANDATORY
    mfrc522.PCD_Init();
    // == Init card reader END ==
    //SIM900_Init();
    MDB_Init();
    sei();
}

void loop() {
    uint16_t Command = 0;
    uint16_t Csh_state  = 0;
    MDB_CommandHandler(&Command, &Csh_state);
//    switch (Command)
//    {        
//        case VMC_RESET     :  Debug.println("VMC_RESET");   break;
//        case VMC_SETUP     :  Debug.println("VMC_SETUP");           break;
//        case VMC_POLL      :  Debug.println("VMC_POLL");            break;
//        case VMC_VEND      :  Debug.println("VMC_VEND");            break;
//        case VMC_READER    :  Debug.println("VMC_READER");          break;
//        case VMC_EXPANSION :  Debug.println("VMC_EXPANSION");       break;
//        default : break;
//    }
    //Debug.println(Command);
    //Debug.println(Csh_state);
    sessionHandler();
    // without this delay READER_ENABLE command won't be in RX Buffer
    // Establish the reason of this later (maybe something is wrong with RX buffer reading)
    if (CSH_GetDeviceState() == CSH_S_DISABLED)
        delay(15);
        Debug.println("CSH_S_DISABLED");
}

/*
 * Handler for two Cashless Device active states:
 * ENABLED -- device is waiting for a new card
 * VEND    -- device is busy making transactions with the server
 */
void sessionHandler(void)
{
    switch(CSH_GetDeviceState())
    {
        case CSH_S_ENABLED      : RFID_readerHandler(); break;
        // check timeout
        case CSH_S_SESSION_IDLE : timeout(global_time, FUNDS_TIMEOUT * 1000); break;
        case CSH_S_VEND         : transactionHandler(); break;
        default : break;
    }
}
/*
 * Waiting for RFID tag routine
 * I a new card is detected and it is present in the server's database,
 * server replies with available funds
 * then set them via functions from MDB.h,
 * and turn Cashless Device Poll Reply as BEGIN_SESSION
 */
void RFID_readerHandler(void)
{
  Debug.println("ReaderHandler");
    String old_uid_str_obj;
    String new_uid_str_obj;
    
    String http_request;
    uint16_t status_code = 0;
    uint16_t user_funds  = 0;

    // Look for new cards
    if ( ! mfrc522.PICC_IsNewCardPresent())
        return;

    // Select one of the cards
    if ( ! mfrc522.PICC_ReadCardSerial())
        return;
//    in_progress = 0;
    // Convert UID into string represented HEX number
    // in reversed order (for human recognition)
    getUIDStrHex(&mfrc522, &new_uid_str_obj);
    
    // Check if there is no UID -- then just return
    if (new_uid_str_obj.length() == 0)
        return;
    // Check if previous UID == UID that was just read
    // to prevent an infinite HTTP GET requests
    if (uid_str_obj == new_uid_str_obj)
        return;
    // Now update the global UID
    uid_str_obj = new_uid_str_obj;
    
#ifdef SOFTWARESERIAL_DEBUG_ENABLED
    Debug.println(uid_str_obj);
#endif

    // HTTP handling
    http_request = server_url_str + uid_str_obj;
    submitHTTPRequest(GET, http_request, &status_code, &user_funds);

#ifdef SOFTWARESERIAL_DEBUG_ENABLED
    Debug.print(F("HTTP Status Code: "));
    Debug.println(status_code);
#endif
    
    if (status_code == 200)
    {
#ifdef SOFTWARESERIAL_DEBUG_ENABLED
        Debug.print(F("Funds: "));
        Debug.println(user_funds);
        Debug.println(F("Poll State: Begin Session"));
#endif
        CSH_SetUserFunds(user_funds); // Set current user funds
        CSH_SetPollState(CSH_BEGIN_SESSION); // Set Poll Reply to BEGIN SESSION
        CSH_SetDeviceState(CSH_S_PROCESSING);
        global_time = millis();
        return;
    }
    // If we got here that means we didn't receive permission for transaction
    // so clear global UID string and global_time
    uid_str_obj = "";
    global_time = 0;
    /*
     * SET FUNDS AND CONNECTION TIMEOUT SOMEWHERE, TO CANCEL SESSION AFTER 10 SECONDS !!!
     */
}
/*
 * Transaction routine
 * At this point item_cost and item_number (or vend_amount)
 * are should be set by VendRequest() (in MDB.c)
 */
void transactionHandler(void)
{
    String http_request;
    uint16_t status_code = 0;
    uint16_t user_funds  = 0;
        
    uint16_t item_cost = CSH_GetItemCost();
    uint16_t item_numb = CSH_GetVendAmount();
    
#ifdef SOFTWARESERIAL_DEBUG_ENABLED
    Debug.println(F("Vend Request"));
#endif

    http_request = server_url_str + uid_str_obj + "/" + String(item_cost);
    uid_str_obj = "";
    global_time = 0;
    submitHTTPRequest(POST, http_request, &status_code, &user_funds);
    
    if (status_code != 202)
    {
        CSH_SetPollState(CSH_VEND_DENIED);
        CSH_SetDeviceState(CSH_S_PROCESSING);
#ifdef SOFTWARESERIAL_DEBUG_ENABLED
        Debug.println(F("Vend Denied"));
#endif
        return;
    }
    // If code is 202 -- tell VMC that vend is approved
    CSH_SetPollState(CSH_VEND_APPROVED);
    CSH_SetDeviceState(CSH_S_PROCESSING);
//    in_progress = 1;
    
#ifdef SOFTWARESERIAL_DEBUG_ENABLED
    Debug.println(F("Vend Approved"));
#endif
}

/*
 * 20-02-2016
 * Convert byte array with size of (4..10) 8-bit hex numbers into a string object
 * Example: array of 4 hex numbers, which represents 32-bit unsigned long:
 * {0x36, 0x6B, 0x1B, 0xDB} represents a 32-bit (4-word) number 0xDB1B6B36, which is converted into a string "63B6B1BD"
 * This string represented hex number appears reversed for human recognition,
 * although technically the least 4-bit hex digit accords to the first letter of a string, uuid_str[0]
 *       and the most significant 4-bit hex digit placed as a last char element (except for '\0')
 *       
 * changes the String &uid_str object by pointer, considered as output
 */
void getUIDStrHex(MFRC522 *card, String *uid_str)
{
    char uuid_str[2 * card->uid.size];
    
    // This cycle makes a conversion of the following manner for each 8-bit number: {0x1B} --> {0x0B, 0x01}
    for (byte i = 0; i < card->uid.size; ++i) {
        uuid_str[2 * i] = card->uid.uidByte[i] & 0b1111;
        uuid_str[2 * i + 1] = card->uid.uidByte[i] >> 4;
    }
    //uuid_str[2 * card->uid.size + 1] = '\0';  // Add a null-terminator to make it a C-style string

    /* 
     *  This cycle adds 0x30 or (0x41 - 0x0A) to actual numbers according to ASCII table 
     *  0x00 to 0x09 digits become '0' to '9' chars (add 0x30)
     *  0x0A to 0x0F digits become 'A' to 'F' chars in UPPERCASE (add 0x41 and subtract 0x0A)
     *  Last thing is to copy that into a String object, which is easier to handle
     */
    for (byte i = 0; i < 2 * card->uid.size; ++i)
    {
        if (uuid_str[i] < 0x0A)
            uuid_str[i] = (uuid_str[i] + 0x30);
        else
            uuid_str[i] = (uuid_str[i] - 0x0A + 0x41);
        *uid_str += uuid_str[i];
    }
}

void setUID(String uid_str)
{
    
}
String getUID(void)
{
    
}

/*
 * Send HTTP request
 * input arguments: enum HTTPMethod -- GET, POST or HEAD
 *                  String request -- request string of structure http://server_address:server_port/path
 *                  uint8_t  *http_status_code -- returned http code by pointer
 *                  uint16_t *user_funds -- returned user funds by pointer
 * returned value:  none
 */
void submitHTTPRequest(HTTP_Method method, String http_link, uint16_t *status_code, uint16_t *user_funds)

{
  *user_funds = 1000;
  switch(method)
    {
      case GET : *status_code = 200;
      case POST : *status_code = 202;
      }
  
  }
//{
//    String response;
//    uint8_t start_index;
//    uint8_t end_index;
////    uint8_t data_len;
//    // Commands for maitainance
////    SIM900_SendCommand(F("AT"));
////    SIM900_SendCommand(F("CPIN?"));
////    SIM900_SendCommand(F("CSQ"));
////    SIM900_SendCommand(F("CREG?"));
////    SIM900_SendCommand(F("CGATT?"));
//    
//    // TCP config
////    SIM900_SendCommand(F("CIPSHUT"));
////    SIM900_SendCommand(F("CSTT=\"internet.beeline.ru\",\"beeline\",\"beeline\""));
////    SIM900_SendCommand(F("CIICR"));
////    SIM900_SendCommand(F("CIFSR"));
////    SIM900_SendCommand(F("CIPSTATUS"));
//
//    // Make connection and HTTP request
//    SIM900_SendCommand(F("SAPBR=1,1")); // Enable bearer
//    SIM900_SendCommand(F("HTTPINIT"));
//    SIM900_SendCommand("HTTPPARA=\"URL\",\"" + http_link + "\""); // cant put these two strings into flash memory
//    SIM900_SendCommand(F("HTTPPARA=\"CID\",1"));
//    
//    switch (method)
//    {   //submit the request
//        case GET  : response = SIM900_SendCommand(F("HTTPACTION=0")); break; // GET
//        case POST : response = SIM900_SendCommand(F("HTTPACTION=1")); break; // POST
////        case HEAD : response = SIM900_SendCommand(F("HTTPACTION=2")); break; // HEAD // not needed for now, disabled to free Flash memory
//        default : return;
//    } // optimize for flash/RAM space, HTTPACTION repeats 3 times
//    /*
//     * Read the HTTP code
//     * Successful response is of the following format (without angular brackets):
//     * OK\r\n+HTTPACTION:<method>,<status_code>,<datalen>\r\n
//     * so search for the first comma
//     */
//    start_index = response.indexOf(',') + 1;
//    end_index = response.indexOf(',', start_index);
//    *status_code = (uint16_t)response.substring(start_index, end_index).toInt();
//    
//    if (*status_code == 200) // read the response
//    {
//        /*
//         * Successful response is of the following format:
//         * AT+HTTPREAD\r\n+HTTPREAD:<datalen>,<data>\r\nOK\r\n
//         * so search for the first ':', read data_len until '\r'
//         * then read the data itself
//         */
//        response = SIM900_SendCommand(F("HTTPREAD"));
//        start_index = response.indexOf(':') + 1;
//        end_index = response.indexOf('\r', start_index);
////        data_len = (uint8_t)response.substring(start_index, end_index).toInt();
//        // now parse string from a new start_index to start_index + data_len
//        start_index = response.indexOf('\n', end_index) + 1;
////        *user_funds = (uint16_t)response.substring(start_index, start_index + data_len).toInt();
////      // or until '\r', which takes less Flash memory
//        end_index = response.indexOf('\r', start_index);
//        *user_funds = (uint16_t)response.substring(start_index, end_index).toInt();
//    }
//    // These two are must be present in the end
//    SIM900_SendCommand(F("HTTPTERM")); // Terminate session, mandatory
//    SIM900_SendCommand(F("SAPBR=0,1")); // Disable bearer, mandatory
//}

void timeout(uint32_t start_time, uint32_t timeout_value)
{
    if (CSH_GetDeviceState() != CSH_S_SESSION_IDLE)
        return;
//    if (in_progress == 1)
//        return;
    if (millis() - start_time >= timeout_value)
        terminateSession();
}
/*
 * Terminates current session and sets device back to ENABLED state
 * This one called when timeout ran out or return button was pressed
 */
void terminateSession(void)
{
    CSH_SetPollState(CSH_SESSION_CANCEL_REQUEST);
    CSH_SetDeviceState(CSH_S_ENABLED);
    uid_str_obj = "";
    global_time = 0;
#ifdef SOFTWARESERIAL_DEBUG_ENABLED
    Debug.println(F("Session terminated"));
#endif
}



#include "MDB.h"
#include "USART.h"
#include <Arduino.h> // For LED debug purposes

#define RX_DEBUG_PIN    2
#define TX_DEBUG_PIN    3

VMC_Config_t vmc_config = {0, 0, 0, 0};
VMC_Prices_t vmc_prices = {0, 0};

 // Available user funds, changed via server connection in the Enabled state
uint16_t user_funds  = 0x0000;
// Selected item price,  changed via VMC_VEND_REQUEST
uint16_t item_cost   = 0x0000;
// Selected item amount, changed via VMC_VEND_REQUEST
// This one is not necessarily the ampunt of selected items to dispense,
// it also can be a single item position value in the VMC memory
uint16_t vend_amount = 0x0000;
uint8_t csh_error_code = 0;
// Cashless Device State and Poll State at a power-up
uint16_t csh_poll_state = CSH_JUST_RESET;
uint8_t csh_state = CSH_S_INACTIVE;
CSH_Config_t csh_config = {
    0x02, // featureLevel
    /*
     * Russia's country code is 810 or 643,
     * which translates into 0x18, 0x10
     *                    or 0x16, 0x43
     */  
    // 0x18, // country code H
    // 0x10, // country code L
    0x19,
    0x78,
    /*
     * The VMC I work with accepts only 2 decimal places,
     * which is reasonable, considering 1 RUB = 100 Kopecks.
     * But actually there are no kopecks
     * in any item prices, rubles only.
     * As a coin acceptor handles coins with minimum value of 1 RUB,
     * I chose scaling factor 0d100 (0x64),
     * which makes calculations easier to understand.
     * Example: Funds available   -- 1600.00 RUB
     *          Scale factor      -- 100
     *          Decimal places    -- 2
     * This makes internal funds value 1600, or 0x0640 in HEX
     * which divides into 0x06 and 0x40
     * for two uint8_t internal variables
     */
    0x01, // Scale Factor 
    0x02, // Decimal Places
    FUNDS_TIMEOUT, // Max Response Time
    0b00000001  // Misc Options
};


void MDB_Init(void)
{
    USART_Init(9600);
}
/*
 *
 */
void MDB_CommandHandler(uint16_t *Command, uint16_t *Csh_state)
{
    uint16_t command = 0;
    // MDB_Read(&command);
    // wait for commands only for our cashless device
    do {
        MDB_Read(&command);
    } while ((command < VMC_RESET) || (command > VMC_EXPANSION));

    switch (command)
    {        
        case VMC_RESET     : /*PORTC ^= (1 << 0);*/ MDB_ResetHandler();     break;
        case VMC_SETUP     : /*PORTC ^= (1 << 1);*/ MDB_SetupHandler();     break;
        case VMC_POLL      : /*PORTC ^= (1 << 2);*/ MDB_PollHandler();      break;
        case VMC_VEND      : /*PORTC ^= (1 << 3);*/ MDB_VendHandler();      break;
        case VMC_READER    : /*PORTC ^= (1 << 4);*/ MDB_ReaderHandler();    break;
        case VMC_EXPANSION : /*PORTC ^= (1 << 5);*/ MDB_ExpansionHandler(); break;
        default : break;
    }
    switch (csh_state)
    {
        case CSH_S_INACTIVE : /*PORTC = 0; PORTC |= (1 << 0);*/ break;
        case CSH_S_DISABLED : /*PORTC = 0; PORTC |= (1 << 1);*/ break;
        case CSH_S_ENABLED  : break;
        case CSH_S_SESSION_IDLE : break;
        case CSH_S_VEND     :  break;
        default : break;
    }
    *Command = command;
    *Csh_state= csh_state;
    // // State Machine Logic
    // switch (csh_state)
    // {
    //     case CSH_S_INACTIVE     : //PORTC = 0; PORTC |= (1 << 0);
    //     {
    //         switch (command)
    //         {
    //             case VMC_RESET     : MDB_ResetHandler();     break; //PORTC ^= (1 << 0); 
    //             case VMC_POLL      : MDB_PollHandler();      break;
    //             case VMC_SETUP     : MDB_SetupHandler();     break;
    //             case VMC_EXPANSION : MDB_ExpansionHandler(); break;
    //             default : break;
    //         }
    //     }; break;
    //     case CSH_S_DISABLED     : //PORTC = 0; PORTC |= (1 << 1);
    //     {
    //         switch (command)
    //         {
    //             case VMC_RESET     : MDB_ResetHandler();     break; //PORTC ^= (1 << 0); 
    //             case VMC_SETUP     : MDB_SetupHandler();     break;
    //             // case VMC_POLL      : csh_poll_state = CSH_ACK; MDB_PollHandler();      break;
    //             case VMC_READER    : MDB_ReaderHandler();    break;
    //             case VMC_EXPANSION : MDB_ExpansionHandler(); break;
    //             default : break;
    //         }
    //     }; break;
    //     case CSH_S_ENABLED      : // PORTC = 0; PORTC |= (1 << 5);
    //     {
    //         switch (command)
    //         {
    //             case VMC_RESET     : MDB_ResetHandler();     break; // PORTC ^= (1 << 0); 
    //             case VMC_POLL      : MDB_PollHandler();      break;
    //             case VMC_VEND      : MDB_VendHandler();      break;
    //             case VMC_READER    : MDB_ReaderHandler();    break;
    //             case VMC_EXPANSION : MDB_ExpansionHandler(); break;
                
    //             default : break;
    //         }
    //     }; break;
    //     case CSH_S_SESSION_IDLE :
    //     {
    //         switch (command)
    //         {
    //             case VMC_RESET     : PORTC ^= (1 << 0); MDB_ResetHandler();     break;
    //             case VMC_SETUP     : PORTC ^= (1 << 1); MDB_SetupHandler();     break;
    //             case VMC_POLL      : PORTC ^= (1 << 2); MDB_PollHandler();      break;
    //             case VMC_VEND      : PORTC ^= (1 << 3); MDB_VendHandler();      break;
    //             case VMC_READER    : PORTC ^= (1 << 4); MDB_ReaderHandler();    break;
    //             case VMC_EXPANSION : PORTC ^= (1 << 5); MDB_ExpansionHandler(); break;
    //             default : break;
    //         }
    //     }; break;
    //     case CSH_S_VEND         : //PORTC = 0; PORTC |= (1 << 4);
    //     {
    //         switch (command)
    //         {

    //         }
    //     }; break;

    //     default : break;
    // }
}
/*
 * Handles Just Reset sequence
 */
void MDB_ResetHandler(void)
{
    uint16_t readCmd; while (1) if (MDB_DataCount() > 0) break; MDB_Read(&readCmd);
    Reset();
}
/*
 * Handles Setup sequence
 */
void MDB_SetupHandler(void)
{
    uint8_t i; // counter
    uint8_t checksum = VMC_SETUP;
    uint16_t vmc_temp;
    uint8_t vmc_data[6];
    /*
     * wait for the whole frame
     * frame structure:
     * 1 subcommand + 4 vmc_config fields + 1 Checksum byte
     * 6 elements total
     */
    while (1)
        if (MDB_DataCount() > 5)
            break;
    // Store frame in array
    for (i = 0; i < 6; ++i)
    {
        MDB_Read(&vmc_temp);
        vmc_data[i] = (uint8_t)(vmc_temp & 0x00FF); // get rid of Mode bit if present
    }
    // calculate checksum excluding last read element, which is a received checksum 
    // for (i = 0; i < 5; ++i)
    checksum += calc_checksum(vmc_data, 5);
    // compare calculated and received checksums
    if (checksum != vmc_data[5])
    {
        MDB_Send(CSH_NAK);
        return; // checksum mismatch, error
    }
    // vmc_data[0] is a SETUP Config Data or Max/Min Prices identifier
    Debug.println(vmc_data[0]);
    switch(vmc_data[0])
    {
        case VMC_CONFIG_DATA : {            
            // Store VMC data
            vmc_config.featureLevel   = vmc_data[1];
            vmc_config.displayColumns = vmc_data[2];
            vmc_config.displayRows    = vmc_data[3];
            vmc_config.displayInfo    = vmc_data[4];
            ConfigInfo();
        }; break;

        case VMC_MAX_MIN_PRICES : {
            // Store VMC Prices
            vmc_prices.maxPrice = ((uint16_t)vmc_data[1] << 8) | vmc_data[2];
            vmc_prices.minPrice = ((uint16_t)vmc_data[3] << 8) | vmc_data[4];
            // Send ACK
            MDB_Send(CSH_ACK);
            // Change state to DISABLED
            csh_state = CSH_S_DISABLED;
        }; break;

        default : break;
    }
}
/*
 * Handles Poll replies
 */
void MDB_PollHandler(void)
{
    switch(csh_poll_state)
    {
        case CSH_ACK                    : MDB_Send(CSH_ACK);      break; // if no data is to send, answer with ACK
        case CSH_SILENCE                :                         break;
        case CSH_JUST_RESET             : JustReset();            break;
        case CSH_READER_CONFIG_INFO     : ConfigInfo();           break;
        case CSH_DISPLAY_REQUEST        : DisplayRequest();       break; // <<<=== Global Display Message
        case CSH_BEGIN_SESSION          : BeginSession();         break; // <<<=== Global User Funds
        case CSH_SESSION_CANCEL_REQUEST : SessionCancelRequest(); break;
        case CSH_VEND_APPROVED          : VendApproved();         break;
        case CSH_VEND_DENIED            : VendDenied();           break;
        case CSH_END_SESSION            : EndSession();           break;
        case CSH_CANCELLED              : Cancelled();            break;
        case CSH_PERIPHERAL_ID          : PeripheralID();         break;
        case CSH_MALFUNCTION_ERROR      : MalfunctionError();     break;
        case CSH_CMD_OUT_OF_SEQUENCE    : CmdOutOfSequence();     break;
        case CSH_DIAGNOSTIC_RESPONSE    : DiagnosticResponse();   break;
        default : break;
    }
}
/*
 *
 */
void MDB_VendHandler(void)
{
    uint16_t subcomm_temp;
    uint8_t subcomm;

    // Wait for Subcommand (1 element total)
    while (1)
        if (MDB_DataCount() > 0)
            break;
    // Store Subcommand in array
    MDB_Read(&subcomm_temp);
    subcomm = (uint8_t)(subcomm_temp & 0x00FF); // get rid of Mode bit if present    
    // Switch through subcommands
    switch(subcomm)
    {
        case VMC_VEND_REQUEST : VendRequest();         break;
        case VMC_VEND_CANCEL  : VendDenied();          break;
        case VMC_VEND_SUCCESS : VendSuccessHandler();  break;
        case VMC_VEND_FAILURE : VendFailureHandler();  break;
        case VMC_VEND_SESSION_COMPLETE : VendSessionComplete(); break;
        case VMC_VEND_CASH_SALE : VendCashSale();      break;
        default : break;
    }
}
/*
 *
 */
void MDB_ReaderHandler(void)
{
    uint8_t i; // counter
    uint8_t checksum = VMC_READER;
    uint16_t reader_temp;
    uint8_t reader_data[2];
    // Wait for Subcommand and checksum (2 elements total)
    while (1)
        if (MDB_DataCount() > 1)
            break;
    // Store received data in array
    for (i = 0; i < 2; ++i)
    {
        MDB_Read(&reader_temp);
        reader_data[i] = (uint8_t)(reader_temp & 0x00FF); // get rid of Mode bit if present
    }
    // Calculate checksum
    checksum += calc_checksum(reader_data, 1);
    // Second element is an incoming checksum, compare it to the calculated one
    if (checksum != reader_data[1])
    {
        MDB_Send(CSH_NAK);
        return; // checksum mismatch, error
    }
    // Look at Subcommand
    switch(reader_data[0])
    {
        case VMC_READER_DISABLE : Disable();   break;
        case VMC_READER_ENABLE  : Enable();    break;
        case VMC_READER_CANCEL  : Cancelled(); break;
        default : break;
    }
}
/*
 *
 */
void MDB_ExpansionHandler(void)
{
  
    uint16_t readCmd = 0;

    MDB_Read(&readCmd);
  
    
    switch(readCmd)
    {
        case VMC_EXPANSION_REQUEST_ID  : ExpansionRequestID();PORTC |= (1<<0); break;
        case VMC_EXPANSION_DIAGNOSTICS : ExpansionDiagnostics(); break;
        // Actually I never got VMC_EXPANSION_DIAGNOSTICS subcommand, so whatever
        default : break;
    }
    // Not yet implemented
}


/*
 * ========================================
 * Start of MDB Bus Communication functions
 * ========================================
 */
void MDB_Send(uint16_t data)
{
    USART_TXBuf_Write(data);
}
/* 
 * Receive MDB Command/Reply/Data from the Bus
 */
void MDB_Read(uint16_t *data)
{
    USART_RXBuf_Read(data);
}
/*
 * Peek at the oldest incoming data from the Bus
 */
void MDB_Peek(uint16_t *data)
{
    USART_RXBuf_Peek(data);
}

uint8_t MDB_DataCount (void)
{
    return USART_RXBuf_Count();
}
/*
 * ======================================
 * End of MDB Bus Communication functions
 * ======================================
 */

/*
 * =============================================
 * Start of Internal functions for main handlers
 * =============================================
 */
void Reset(void)
{
    // Reset all data
    vmc_config.featureLevel   = 0;
    vmc_config.displayColumns = 0;
    vmc_config.displayRows    = 0;
    vmc_config.displayInfo    = 0;

    vmc_prices.maxPrice = 0;
    vmc_prices.minPrice = 0;

    csh_error_code = 0;
    // Send ACK, turn INACTIVE
    csh_state = CSH_S_INACTIVE;
    csh_poll_state = CSH_JUST_RESET;
    uint16_t readCmd; while (1) if (MDB_DataCount() > 0) break; MDB_Read(&readCmd);
    MDB_Send(CSH_ACK);

    while ( ! (USART_TXBuf_IsEmpty()) )
        ;
}

void JustReset(void)
{
    uint16_t readCmd; while (1) if (MDB_DataCount() > 0) break; MDB_Read(&readCmd);
    MDB_Send(CSH_JUST_RESET);
    Reset();

    while ( ! (USART_TXBuf_IsEmpty()) )
        ;
}



void ConfigInfo(void)
{
    uint8_t checksum = 0;
    // calculate checksum, no Mode bit yet
    checksum = ( CSH_READER_CONFIG_INFO
               + csh_config.featureLevel
               + csh_config.countryCodeH
               + csh_config.countryCodeL
               + csh_config.scaleFactor
               + csh_config.decimalPlaces
               + csh_config.maxResponseTime
               + csh_config.miscOptions );
    MDB_Send(CSH_READER_CONFIG_INFO);
    MDB_Send(csh_config.featureLevel);
    MDB_Send(csh_config.countryCodeH);
    MDB_Send(csh_config.countryCodeL);
    MDB_Send(csh_config.scaleFactor);
    MDB_Send(csh_config.decimalPlaces);
    MDB_Send(csh_config.maxResponseTime);
    MDB_Send(csh_config.miscOptions);
    MDB_Send(checksum | CSH_ACK);

    while ( ! (USART_TXBuf_IsEmpty()) )
        ;
}

void DisplayRequest(void)
{
    // char *message;
    // uint8_t i;
    // uint8_t checksum = CSH_DISPLAY_REQUEST;
    // uint8_t message_length;
    // // As in standard, the number of bytes must equal the product of Y3 and Y4
    // // up to a maximum of 32 bytes in the setup/configuration command
    // message_length = (vmc_config.displayColumns * vmc_config.displayRows);
    // message = (char *)malloc(message_length * sizeof(char));

    
    //  /* Here you need to copy the message into allocated memory
     

    // // Send al this on the Bus
    // MDB_Send(CSH_DISPLAY_REQUEST);
    // MDB_Send(0xAA); // Display time in arg * 0.1 second units
    // MDB_Send(0x34);
    // MDB_Send(0x38);
    // checksum += (0xAA + 0x34 + 0x38);
    // MDB_Send(checksum | CSH_ACK);

    // for(i = 0; i < message_length; ++i)
    //     MDB_Send(*(message + i));
    // free(message);
}

void BeginSession(void) // <<<=== Global User Funds
{
    /*
     * I need this variable because my VMC doesn't want to send a
     * VMC_VEND command (0x113) after I answer it with a single BEGIN SESSION Poll Reply
     * So I decided to try it several times, that is what this counter for
     */
    static uint8_t begin_session_counter = 0;
    // uint16_t reply;
    uint8_t checksum = 0;
    uint16_t funds = CSH_GetUserFunds();
    uint8_t user_funds_H = (uint8_t)(funds >> 8);
    uint8_t user_funds_L = (uint8_t)(funds &  0x00FF);

    // Again, that ugly counter, experimentally established, that 10 times is enough
    begin_session_counter++;
    if (begin_session_counter >= 50)
    {
        csh_poll_state = CSH_ACK;
        csh_state = CSH_S_SESSION_IDLE;
        begin_session_counter = 0;
        return;
    }

    // calculate checksum, no Mode bit yet
    checksum = ( CSH_BEGIN_SESSION
               + user_funds_H
               + user_funds_L );
    
    MDB_Send(CSH_BEGIN_SESSION);
    MDB_Send(user_funds_H);      // upper byte of funds available
    MDB_Send(user_funds_L);      // lower byte of funds available
    MDB_Send(checksum | CSH_ACK);   // set Mode bit and send    
    
    while ( ! (USART_TXBuf_IsEmpty()) )
        ;
        // while (1)
        //     if (MDB_DataCount() > 0)
        //         break;
    // do {
    //     MDB_Read(&reply);
    // } while (reply != CSH_ACK);
    // Initiate Session Idle State
}

void SessionCancelRequest(void)
{
    MDB_Send(CSH_SESSION_CANCEL_REQUEST);
    MDB_Send(CSH_SESSION_CANCEL_REQUEST | CSH_ACK); // Checksum with Mode bit

    while ( ! (USART_TXBuf_IsEmpty()) )
        ;
}

void VendApproved(void)
{
    static uint8_t vend_approved_counter = 0;
    uint8_t checksum = 0;
    uint8_t vend_amount_H = (uint8_t)(vend_amount >> 8);
    uint8_t vend_amount_L = (uint8_t)(vend_amount &  0x00FF);
    
    vend_approved_counter++;
    if (vend_approved_counter >= 50)
    {
        csh_poll_state = CSH_END_SESSION;
        vend_approved_counter = 0;
        return;
    }
    // calculate checksum, no Mode bit yet
    checksum = ( CSH_VEND_APPROVED
               + vend_amount_H
               + vend_amount_L );
    MDB_Send(CSH_VEND_APPROVED);
    MDB_Send(vend_amount_H);       // Vend Amount H
    MDB_Send(vend_amount_L);       // Vend Amount L
    MDB_Send(checksum | CSH_ACK);  // set Mode bit and send
     
    while ( ! (USART_TXBuf_IsEmpty()) )
        ;
}

void VendDenied(void)
{
    MDB_Send(CSH_VEND_DENIED);
    MDB_Send(CSH_VEND_DENIED | CSH_ACK); // Checksum with Mode bit set

    while ( ! (USART_TXBuf_IsEmpty()) )
        ;
    csh_poll_state = CSH_END_SESSION;
    // // just for safety, this may be redundant
    // item_cost   = 0;
    // vend_amount = 0;
}

void EndSession(void)
{
    MDB_Send(CSH_END_SESSION);
    MDB_Send(CSH_END_SESSION | CSH_ACK); // Checksum with Mode bit

    // while ( ! (USART_TXBuf_IsEmpty()) )
    //         ;
    // Null data of the ended session
    CSH_SetUserFunds(0);
    CSH_SetItemCost(0);
    CSH_SetVendAmount(0);   
    CSH_SetPollState(CSH_JUST_RESET);
    CSH_SetDeviceState(CSH_S_ENABLED); // csh_state = CSH_S_ENABLED;
}

void Cancelled(void)
{
    if (csh_state != CSH_S_ENABLED)
        return;
    MDB_Send(CSH_CANCELLED);
    MDB_Send(CSH_CANCELLED | CSH_ACK); // Checksum with Mode bit
    while ( ! (USART_TXBuf_IsEmpty()) )
        ;
}

void PeripheralID(void)
{
    // Not fully implemented yet
    // Manufacturer Data, 30 bytes + checksum with mode bit
    uint8_t i; // counter
    uint8_t checksum = 0;
    uint8_t periph_id[31];
    uint8_t a_char = 0x41;
    periph_id[0] = CSH_PERIPHERAL_ID;
    periph_id[1] = 'U'; periph_id[2] = 'N'; periph_id[3] = 'I'; // Unicum manufacturer, Russia
    
    // Set Manufacturer ID 000000000001
    for (i = 4; i < 15; ++i)
        periph_id[i] = 0;

    periph_id[15] = 1;

    // Set Serial Number, ASCII
    // Set Model Number, ASCII
    for (i = 16; i < 28; ++i)
        periph_id[i] = a_char + i;
    // Set Software verion, packed BCD 
    periph_id[28] = 1;
    periph_id[29] = 0;
    periph_id[30] = calc_checksum(periph_id, 29);
    // Send all data on the bus
    for (i = 0; i < 30; ++i)
        MDB_Send(periph_id[i]);

    MDB_Send(periph_id[30] | CSH_ACK);

    while ( ! (USART_TXBuf_IsEmpty()) )
        ;
}

void MalfunctionError(void)
{
    uint8_t i;
    uint8_t malf_err[2];
    malf_err[0] = CSH_MALFUNCTION_ERROR;
    malf_err[1] = csh_error_code;
    // calculate checksum, set Mode bit and store it
    malf_err[2] = calc_checksum(malf_err, 2) | CSH_ACK;
    for (i = 0; i < 3; ++i)
        MDB_Send(malf_err[i]);
}

void CmdOutOfSequence(void)
{
    MDB_Send(CSH_CMD_OUT_OF_SEQUENCE);
    MDB_Send(CSH_CMD_OUT_OF_SEQUENCE | CSH_ACK);
}

void DiagnosticResponse(void)
{

}

/*
 * Internal functions for MDB_VendHandler()
 */
void VendRequest(void)
{
    uint8_t i; // counter
    uint8_t checksum = VMC_VEND + VMC_VEND_REQUEST;
    uint8_t vend_data[5];
    uint16_t vend_temp;
    // Wait for 5 elements in buffer
    // 4 data + 1 checksum
    while (1)
        if (MDB_DataCount() > 4)
            break;
    // Read all data and store it in an array, with a subcommand
    for (i = 0; i < 5; ++i)
    {
        MDB_Read(&vend_temp);
        vend_data[i] = (uint8_t)(vend_temp & 0x00FF); // get rid of Mode bit if present
    }
    // calculate checksum excluding last read element, which is a received checksum
    checksum += calc_checksum(vend_data, 4);
    // compare calculated and received checksums
    if (checksum != vend_data[4])
    {
        MDB_Send(CSH_NAK);
        return; // checksum mismatch, error
    }
    CSH_SetItemCost((vend_data[0] << 8) | vend_data[1]);
    CSH_SetVendAmount((vend_data[2] << 8) | vend_data[3]);
    // Send ACK to VMC
    MDB_Send(CSH_ACK);
    // Set uninterruptable VEND state
    csh_state = CSH_S_VEND;
    /*
     * ===================================
     * HERE GOES THE CODE FOR RFID/HTTP Handlers
     * if enough payment media, tell server to subtract sum
     * wait for server reply, then CSH_VEND_APPROVED
     * Otherwise, CSH_VEND_DENIED
     * ===================================
    */
}

void VendSuccessHandler(void)
{
    uint8_t i; // counter
    uint8_t checksum = VMC_VEND + VMC_VEND_SUCCESS;
    uint8_t vend_data[3];
    uint16_t vend_temp;

    // Wait for 3 elements in buffer
    // 2 data + 1 checksum
    while (1)
        if (MDB_DataCount() > 2)
            break;

    for (i = 0; i < 3; ++i)
    {
        MDB_Read(&vend_temp);
        vend_data[i] = (uint8_t)(vend_temp & 0x00FF); // get rid of Mode bit if present
    }
    /* here goes another check-check with a server */
    MDB_Send(CSH_ACK);

    // Return state to SESSION IDLE
    csh_state = CSH_S_SESSION_IDLE;

    // checksum += calc_checksum(vend_data, 2);
    // if (checksum != vend_data[2])
    // {
    //     MDB_Send(CSH_NAK);
    //     return; // checksum mismatch, error
    // }
}

void VendFailureHandler(void)
{
    uint8_t checksum = VMC_VEND + VMC_VEND_FAILURE;
    uint8_t incoming_checksum;
    uint16_t temp;
    // Wait for 1 element in buffer
    // 1 checksum
    while (1)
        if (MDB_DataCount() > 0)
            break;
    MDB_Read(&temp);
    incoming_checksum = (uint8_t)(temp & 0x00FF); // get rid of Mode bit if present
    if (checksum != incoming_checksum)
    {
        MDB_Send(CSH_NAK);
        return; // checksum mismatch, error
    }
    /* refund through server connection */

    MDB_Send(CSH_ACK); // in case of success
    // MalfunctionError(); -- in case of failure, like unable to connect to server
    // Return state to SESSION IDLE
    csh_state = CSH_S_SESSION_IDLE;
}

void VendSessionComplete(void)
{
    MDB_Send(CSH_ACK);
    CSH_SetPollState(CSH_END_SESSION);
}

void VendCashSale(void)
{
    uint8_t i; // counter
    uint8_t checksum = VMC_VEND + VMC_VEND_CASH_SALE;
    uint8_t vend_data[5];
    uint16_t vend_temp;
    // Wait for 5 elements in buffer
    // 4 data + 1 checksum
    while (1)
        if (MDB_DataCount() > 4)
            break;
    for (i = 0; i < 5; ++i)
    {
        MDB_Read(&vend_temp);
        vend_data[i] = (uint8_t)(vend_temp & 0x00FF); // get rid of Mode bit if present
    }
    checksum += calc_checksum(vend_data, 4);
    if (checksum != vend_data[4])
    {
        MDB_Send(CSH_NAK);
        return; // checksum mismatch, error
    }

    /* Cash sale implementation */

    MDB_Send(CSH_ACK);
}
/*
 * Internal functions for MDB_ReaderHandler()
 */
void Disable(void)
{
    csh_state = CSH_S_DISABLED;
    MDB_Send(CSH_ACK);
}

void Enable(void)
{
    if (csh_state != CSH_S_DISABLED)
        return;
    csh_state = CSH_S_ENABLED;
    csh_poll_state = CSH_ACK;
    MDB_Send(CSH_ACK);
}
/*
 * Internal functions for MDB_ExpansionHandler()
 */
void ExpansionRequestID(void)
{
  
    uint16_t i; // counter
    uint8_t checksum = VMC_EXPANSION + VMC_EXPANSION_REQUEST_ID;
    uint16_t temp;
    uint8_t data[30];
    /*
     * Wait for incoming 29 data elements + 1 checksum (30 total)
     * Store the data by the following indexes:
     *  0,  1,  2 -- Manufacturer Code (3 elements)
     *  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14 -- Serial Number (12 elements)
     * 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26 -- Model  Number (12 elements)
     * 27, 28 -- Software Version (2 elements)
     * 29 -- Checksum (1 element)
     */
    while (1)
        if (MDB_DataCount() > 29)
            break;
    // Store data
    PORTC |= (1 << 1);
    for (i = 0; i < 30; ++i)
    {
        MDB_Read(&temp);
        data[i] = (uint8_t)(temp & 0x00FF); // get rid of Mode bit if present
    }

    // Calculate checksum
    checksum += calc_checksum(data, 29);
    // Second element is an incoming checksum, compare it to the calculated one
    if (checksum != data[29])
    {
        MDB_Send(CSH_NAK);
        
        
        return; // checksum mismatch, error
    }
    // Respond with our own data
    
    PeripheralID();
    
    while ( ! (USART_TXBuf_IsEmpty()) )
        ;
}

void ExpansionDiagnostics(void)
{
    uint8_t checksum = VMC_EXPANSION + VMC_EXPANSION_DIAGNOSTICS;
    MDB_Send(CSH_ACK);
    // while(1)
    //     if (MDB_DataCount() > )
}
/*
 * =============================================
 *   End of Internal functions for main handlers
 * =============================================
 */
/*
 * Functions to work with Cashless device states and data
 */
uint8_t CSH_GetPollState(void)
{
    return csh_poll_state;
}
uint8_t CSH_GetDeviceState(void)
{
    return csh_state;
}
uint16_t CSH_GetUserFunds(void)
{
    return user_funds;
}
uint16_t CSH_GetItemCost(void)
{
    return item_cost;
}
uint16_t CSH_GetVendAmount(void)
{
    return vend_amount;
}
void CSH_SetPollState(uint8_t new_poll_state)
{
    csh_poll_state = new_poll_state;
}
void CSH_SetDeviceState(uint8_t new_device_state)
{
    csh_state = new_device_state;
}
void CSH_SetUserFunds(uint16_t new_funds)
{
    user_funds = new_funds;
}
void CSH_SetItemCost(uint16_t new_item_cost)
{
    item_cost = new_item_cost;
}
void CSH_SetVendAmount(uint16_t new_vend_amount)
{
    vend_amount = new_vend_amount;
}

/*
 * Misc helper functions
 */

 /*
  * calc_checksum()
  * Calculates checksum of *array from 0 to arr_size
  * Use with caution (because of pointer arithmetics)
  */
uint8_t calc_checksum(uint8_t *array, uint8_t arr_size)
{
    uint8_t ret_val = 0x00;
    uint8_t i;
    for (i = 0; i < arr_size; ++i)
        ret_val += *(array + i);
    return ret_val;
}