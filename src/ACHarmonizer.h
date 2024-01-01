// ACHarmonizer.h


// Set base?
// Send single note?

#define SCALE_MAX       20
#define SCALE_DEFAULT   0
#define BASE_DEFAULT    1

class ACHarmonizer {
public:
    ACHarmonizer();
    void begin();
    void setBase(unsigned char value);
    void setScale(unsigned char value);
    void toggleScale(unsigned char direction);
    unsigned char harmonize(unsigned char value, unsigned char scale);

private:
    volatile unsigned char harmonizer_base;
    volatile unsigned char harmonizer_scale;
    volatile unsigned char harmonizer_listen;
    static const unsigned char scale_set[SCALE_MAX][12];

};

