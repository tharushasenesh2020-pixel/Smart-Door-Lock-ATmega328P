#define F_CPU 16000000UL
#include <avr/io.h>
#include <util/delay.h>
#include <avr/eeprom.h>
#include <string.h>
#include <stdbool.h>

/* ------------------- Config ------------------- */
#define SOLENOID_PORT PORTB
#define SOLENOID_DDR  DDRB
#define SOLENOID_PIN  PB1     // Solenoid pin
#define LED_PIN       PB0     // Optional LED feedback pin

#define MAX_WRONG_ATTEMPTS 3
#define LOCKOUT_MS         10000
#define UNLOCK_MS          5000

#define EE_MAGIC_ADDR  ((uint8_t*)0)
#define EE_PIN_ADDR    ((uint8_t*)1)    // 4 bytes for PIN digits
#define EE_MAGIC_VALUE 0xA5

/* ------------------- I2C LCD ------------------- */
#define LCD_ADDR 0x27
#define LCD_BACKLIGHT 0x08
#define LCD_EN 0x04
#define LCD_RS 0x01

void i2c_init(void){
    TWSR = 0x00;
    TWBR = 72; // ~100kHz at 16MHz
}

void i2c_start(uint8_t addr){
    TWCR = (1<<TWINT)|(1<<TWSTA)|(1<<TWEN);
    while(!(TWCR & (1<<TWINT)));
    TWDR = addr;
    TWCR = (1<<TWINT)|(1<<TWEN);
    while(!(TWCR & (1<<TWINT)));
}

void i2c_write(uint8_t data){
    TWDR = data;
    TWCR = (1<<TWINT)|(1<<TWEN);
    while(!(TWCR & (1<<TWINT)));
}

void i2c_stop(void){
    TWCR = (1<<TWINT)|(1<<TWEN)|(1<<TWSTO);
    while(TWCR & (1<<TWSTO));
}

static void lcd_i2c_write(uint8_t data){
    i2c_start((LCD_ADDR<<1)|0);
    i2c_write(data | LCD_BACKLIGHT);
    i2c_stop();
}

static void lcd_pulse(uint8_t data){
    lcd_i2c_write(data | LCD_EN);
    _delay_us(1);
    lcd_i2c_write(data & ~LCD_EN);
    _delay_us(50);
}

static void lcd_send(uint8_t val, uint8_t mode){
    uint8_t high = val & 0xF0;
    uint8_t low  = (val<<4) & 0xF0;
    lcd_pulse(high | mode);
    lcd_pulse(low | mode);
}

static void lcd_cmd(uint8_t cmd){
    lcd_send(cmd, 0);
    if(cmd==0x01 || cmd==0x02) _delay_ms(2);
}

static void lcd_data(uint8_t data){
    lcd_send(data, LCD_RS);
}

static void lcd_gotoxy(uint8_t x, uint8_t y){
    lcd_cmd((y==0)?0x80+x:0xC0+x);
}

static void lcd_puts(const char* s){
    while(*s) lcd_data(*s++);
}

static void lcd_init(void){
    _delay_ms(50);
    lcd_pulse(0x30); _delay_ms(5);
    lcd_pulse(0x30); _delay_us(200);
    lcd_pulse(0x30); _delay_us(200);
    lcd_pulse(0x20);
    lcd_cmd(0x28);
    lcd_cmd(0x0C);
    lcd_cmd(0x06);
    lcd_cmd(0x01);
    _delay_ms(2);
}

static void lcd_print2(const char* l1,const char* l2){
    lcd_cmd(0x01);
    lcd_gotoxy(0,0); lcd_puts(l1);
    lcd_gotoxy(0,1); lcd_puts(l2);
}

/* ------------------- Hardware UART (PD0=RX, PD1=TX) ------------------- */
#define UART_BAUD 9600UL
static void uart_init(void){
    uint16_t ubrr = (F_CPU/16/UART_BAUD)-1;
    UBRR0H = ubrr>>8;
    UBRR0L = ubrr;
    UCSR0B = (1<<RXEN0)|(1<<TXEN0); // Enable RX and TX
    UCSR0C = (1<<UCSZ01)|(1<<UCSZ00); // 8-bit data
}

static char uart_read_nonblocking(void){
    if(UCSR0A & (1<<RXC0)){
        return UDR0;
    }
    return -1;
}

static void uart_tx(uint8_t d){
    while(!(UCSR0A & (1<<UDRE0)));
    UDR0 = d;
}

/* ------------------- Keypad 4x3 ------------------- */
static const char KEYS[4][3]={
    {'1','2','3'},
    {'4','5','6'},
    {'7','8','9'},
    {'*','0','#'}
};

static void keypad_init(void){
    DDRC |= 0x0F; // Rows output
    PORTC |= 0x0F; // Pull-up
    DDRB &= ~(1<<PB2); PORTB |= (1<<PB2); // Column input
    DDRD &= ~((1<<PD6)|(1<<PD7)); PORTD |= (1<<PD6)|(1<<PD7);
}

static char keypad_getkey_blocking(void){
    while(1){
        for(uint8_t row=0; row<4; row++){
            PORTC |= 0x0F;
            PORTC &= ~(1<<row);
            _delay_us(5);
            if(!(PINB&(1<<PB2))){while(!(PINB&(1<<PB2))); return KEYS[row][0];}
            if(!(PIND&(1<<PD6))){while(!(PIND&(1<<PD6))); return KEYS[row][1];}
            if(!(PIND&(1<<PD7))){while(!(PIND&(1<<PD7))); return KEYS[row][2];}
        }
    }
}

/* ------------------- EEPROM PIN ------------------- */
static void eeprom_init_pin(void){
    if(eeprom_read_byte(EE_MAGIC_ADDR)!=EE_MAGIC_VALUE){
        char def[4]={'7','7','7','7'};
        for(uint8_t i=0;i<4;i++) eeprom_write_byte(EE_PIN_ADDR+i,def[i]);
        eeprom_write_byte(EE_MAGIC_ADDR,EE_MAGIC_VALUE);
    }
}

static bool verify_pin(void){
    char stored[4], entered[4];
    for(uint8_t i=0;i<4;i++) stored[i]=eeprom_read_byte(EE_PIN_ADDR+i);
    lcd_print2("Enter PIN:","____"); lcd_gotoxy(0,1);
    for(uint8_t i=0;i<4;i++){
        char k=keypad_getkey_blocking();
        if(k<'0'||k>'9'){i--;continue;}
        entered[i]=k; lcd_data('*');
    }
    return (memcmp(stored,entered,4)==0);
}

/* ------------------- Solenoid ------------------- */
static void solenoid_init(void){
    SOLENOID_DDR |= (1<<SOLENOID_PIN);
    SOLENOID_PORT &= ~(1<<SOLENOID_PIN);
}
static void solenoid_unlock_ms(uint16_t ms){
    SOLENOID_PORT |= (1<<SOLENOID_PIN);
    while(ms--) _delay_ms(1);
    SOLENOID_PORT &= ~(1<<SOLENOID_PIN);
}

/* ------------------- LED ------------------- */
static void blink_led(void){
    PORTB |= (1<<LED_PIN);
    _delay_ms(50);
    PORTB &= ~(1<<LED_PIN);
}

/* ------------------- Main ------------------- */
int main(void){
    i2c_init();
    lcd_init();
    uart_init();
    keypad_init();
    solenoid_init();
    eeprom_init_pin();
    DDRB |= (1<<LED_PIN);

    lcd_print2("Door Lock v6.0","Keypad+BT Ready");
    _delay_ms(1500);

    uint8_t wrong=0;

    while(1){
        lcd_print2("Enter PIN or","BT 1=Unlock 0=Lock");

        // Keypad PIN
        char k=keypad_getkey_blocking();
        if(k=='4'){ // PIN entry
            if(verify_pin()){
                lcd_print2("Correct PIN","Unlocking");
                solenoid_unlock_ms(UNLOCK_MS);
                wrong=0;
            } else {
                lcd_print2("Wrong PIN","");
                _delay_ms(1000);
                wrong++;
            }
        }

        // Bluetooth control
        char btchar = uart_read_nonblocking();
        if(btchar != -1){
            blink_led();
            if(btchar=='1'){ // Unlock
                lcd_print2("BT Unlock","Access Granted");
                solenoid_unlock_ms(UNLOCK_MS);
                wrong=0;
            } else if(btchar=='0'){ // Lock
                lcd_print2("BT Lock","Closed");
                SOLENOID_PORT &= ~(1<<SOLENOID_PIN);
            }
        }

        // Lockout after wrong attempts
        if(wrong >= MAX_WRONG_ATTEMPTS){
            lcd_print2("LOCKED OUT","Wait...");
            for(uint16_t t=0; t<LOCKOUT_MS/250; t++) _delay_ms(250);
            wrong=0;
        }
    }
}

