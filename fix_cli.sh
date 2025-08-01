#!/bin/bash

# Fix CLI files by copying working structure from preset CLI
# and adapting delay functions

for variant in mars moon; do
    echo "Fixing ${variant} CLI..."
    
    # Copy working preset CLI as base
    cp udppresetdelaycli.c udp${variant}delaycli_temp.c
    
    # Replace preset-specific parts with variant-specific parts
    if [ "$variant" = "mars" ]; then
        # Replace Mars-specific constants and functions
        sed -i 's/udppresetdelaycli/udpmarsdelaycli/g' udp${variant}delaycli_temp.c
        sed -i 's/getPresetDelay/calculateMarsDelay/g' udp${variant}delaycli_temp.c
        sed -i 's/preset delay/Mars delay/g' udp${variant}delaycli_temp.c
        
        # Add Mars-specific constants and delay function
        sed -i '/^#include "dtn2fw.h"/a\\n/* Mars delay constants */\
#define SPEED_OF_LIGHT 299792.458          /* km/s */\
#define EARTH_ORBITAL_RADIUS  149598000.0  /* km, 1 AU */\
#define MARS_ORBITAL_RADIUS   227939200.0  /* km, 1.52 AU */\
' udp${variant}delaycli_temp.c
        
        # Replace preset delay function with Mars delay function
        sed -i '/\/\* Get preset delay \*\//,/^}/c\
/* Calculate Mars delay based on current orbital positions */\
static double calculateMarsDelay(void)\
{\
\ttime_t now = time(NULL);\
\tdouble earthAngle, marsAngle;\
\tdouble earthX, earthY, marsX, marsY;\
\tdouble distance;\
\t\
\t/* Simple orbital model - Earth completes orbit in ~365 days, Mars in ~687 days */\
\tearthAngle = fmod((double)(now / 86400.0) * 2.0 * M_PI / 365.25, 2.0 * M_PI);\
\tmarsAngle = fmod((double)(now / 86400.0) * 2.0 * M_PI / 687.0, 2.0 * M_PI);\
\t\
\t/* Calculate positions */\
\tearthX = EARTH_ORBITAL_RADIUS * cos(earthAngle);\
\tearthY = EARTH_ORBITAL_RADIUS * sin(earthAngle);\
\tmarsX = MARS_ORBITAL_RADIUS * cos(marsAngle);\
\tmarsY = MARS_ORBITAL_RADIUS * sin(marsAngle);\
\t\
\t/* Calculate distance */\
\tdistance = sqrt((marsX - earthX) * (marsX - earthX) + \
\t\t\t(marsY - earthY) * (marsY - earthY));\
\t\
\t/* Convert to light-travel time */\
\treturn distance / SPEED_OF_LIGHT;\
}' udp${variant}delaycli_temp.c

        # Remove preset delay constants section
        sed -i '/\/\* Preset delay in seconds/,/^$/d' udp${variant}delaycli_temp.c
        
    elif [ "$variant" = "moon" ]; then
        # Replace Moon-specific constants and functions
        sed -i 's/udppresetdelaycli/udpmoondelaycli/g' udp${variant}delaycli_temp.c
        sed -i 's/getPresetDelay/calculateMoonDelay/g' udp${variant}delaycli_temp.c
        sed -i 's/preset delay/Moon delay/g' udp${variant}delaycli_temp.c
        
        # Add Moon-specific constants and delay function
        sed -i '/^#include "dtn2fw.h"/a\\n/* Moon delay constants */\
#define SPEED_OF_LIGHT 299792.458     /* km/s */\
#define MOON_AVG_DISTANCE 384400.0    /* km */\
#define MOON_VARIATION 20000.0        /* km, variation range */\
' udp${variant}delaycli_temp.c
        
        # Replace preset delay function with Moon delay function
        sed -i '/\/\* Get preset delay \*\//,/^}/c\
/* Calculate Moon delay based on current orbital position */\
static double calculateMoonDelay(void)\
{\
\ttime_t now = time(NULL);\
\tdouble moonPhase;\
\tdouble distance;\
\t\
\t/* Simple lunar orbit model - Moon completes orbit in ~27.3 days */\
\tmoonPhase = fmod((double)(now / 86400.0) * 2.0 * M_PI / 27.3, 2.0 * M_PI);\
\t\
\t/* Calculate distance with sinusoidal variation */\
\tdistance = MOON_AVG_DISTANCE + MOON_VARIATION * sin(moonPhase);\
\t\
\t/* Convert to light-travel time */\
\treturn distance / SPEED_OF_LIGHT;\
}' udp${variant}delaycli_temp.c

        # Remove preset delay constants section
        sed -i '/\/\* Preset delay in seconds/,/^$/d' udp${variant}delaycli_temp.c
    fi
    
    # Move the temp file to the final location
    mv udp${variant}delaycli_temp.c udp${variant}delaycli.c
    
    echo "Fixed ${variant} CLI"
done

echo "All CLI files fixed!"