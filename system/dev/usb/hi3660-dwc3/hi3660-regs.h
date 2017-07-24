
#define USB_REFCLK_ISO_EN       (1 << 25)

#define PERI_CRG_CLK_EN4        (0x40)
#define PERI_CRG_CLK_DIS4       (0x44)
#define PERI_CRG_RSTDIS4        (0x94)
#define PERI_CRG_RSTEN4         (0x90)
#define PERI_CRG_ISODIS         (0x148)
#define PERI_CRG_ISOSTAT        (0x14C)
#define SC_CLK_USB3PHY_3MUX1_SEL        (1 << 25)

#define PCTRL_PERI_CTRL3                0x10

#define USB_TCXO_EN                     (1 << 1)
#define PERI_CTRL3_MSK_START            (16)

#define PCTRL_PERI_CTRL24				(0x64)

#define GT_CLK_USB3OTG_REF				(1 << 0)
#define GT_ACLK_USB3OTG					(1 << 1)
#define GT_CLK_USB3PHY_REF				(1 << 2)

#define USBOTG3_CTRL0		0x00
#define USBOTG3_CTRL1       0x04
#define USBOTG3_CTRL2       0x08
#define USBOTG3_CTRL3       0x0C
#define USBOTG3_CTRL4       0x10
#define USBOTG3_CTRL5       0x14
#define USBOTG3_CTRL6       0x18
#define USBOTG3_CTRL7       0x1C
#define USBOTG3_STS0        0x20
#define USBOTG3_STS1        0x24
#define USBOTG3_STS2        0x28
#define USBOTG3_STS3        0x2C
#define BC_CTRL0            0x30
#define BC_CTRL1            0x34
#define BC_CTRL2            0x38
#define BC_STS0             0x3C
#define RAM_CTRL            0x40
#define USBOTG3_STS4        0x44
#define USB3PHY_CTRL        0x48
#define USB3PHY_STS         0x4C
#define USB3PHY_CR_STS      0x50
#define USB3PHY_CR_CTRL     0x54
#define USB3_RES            0x58
