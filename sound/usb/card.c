/*
 *   (Tentative) USB Audio Driver for ALSA
 *
 *   Copyright (c) 2002 by Takashi Iwai <tiwai@suse.de>
 *
 *   Many codes borrowed from audio.c by
 *	    Alan Cox (alan@lxorguk.ukuu.org.uk)
 *	    Thomas Sailer (sailer@ife.ee.ethz.ch)
 *
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA
 *
 *
 *  NOTES:
 *
 *   - the linked URBs would be preferred but not used so far because of
 *     the instability of unlinking.
 *   - type II is not supported properly.  there is no device which supports
 *     this type *correctly*.  SB extigy looks as if it supports, but it's
 *     indeed an AC3 stream packed in SPDIF frames (i.e. no real AC3 stream).
 */


#include <linux/bitops.h>
#include <linux/init.h>
#include <linux/list.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/ctype.h>
#include <linux/usb.h>
#include <linux/moduleparam.h>
#include <linux/mutex.h>
#include <linux/usb/audio.h>
#include <linux/usb/audio-v2.h>
#include <linux/module.h>

#include <linux/usb/exynos_usb_audio.h>
#include <sound/control.h>
#include <sound/core.h>
#include <sound/info.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/initval.h>

#include "usbaudio.h"
#include "card.h"
#include "midi.h"
#include "mixer.h"
#include "proc.h"
#include "quirks.h"
#include "endpoint.h"
#include "helper.h"
#include "debug.h"
#include "pcm.h"
#include "format.h"
#include "power.h"
#include "stream.h"

MODULE_AUTHOR("Takashi Iwai <tiwai@suse.de>");
MODULE_DESCRIPTION("USB Audio");
MODULE_LICENSE("GPL");
MODULE_SUPPORTED_DEVICE("{{Generic,USB Audio}}");


static int index[SNDRV_CARDS] = SNDRV_DEFAULT_IDX;	/* Index 0-MAX */
static char *id[SNDRV_CARDS] = SNDRV_DEFAULT_STR;	/* ID for this card */
static bool enable[SNDRV_CARDS] = SNDRV_DEFAULT_ENABLE_PNP;/* Enable this card */
/* Vendor/product IDs for this card */
static int vid[SNDRV_CARDS] = { [0 ... (SNDRV_CARDS-1)] = -1 };
static int pid[SNDRV_CARDS] = { [0 ... (SNDRV_CARDS-1)] = -1 };
static int device_setup[SNDRV_CARDS]; /* device parameter for this card */
static bool ignore_ctl_error;
static bool autoclock = true;
static char *quirk_alias[SNDRV_CARDS];

module_param_array(index, int, NULL, 0444);
MODULE_PARM_DESC(index, "Index value for the USB audio adapter.");
module_param_array(id, charp, NULL, 0444);
MODULE_PARM_DESC(id, "ID string for the USB audio adapter.");
module_param_array(enable, bool, NULL, 0444);
MODULE_PARM_DESC(enable, "Enable USB audio adapter.");
module_param_array(vid, int, NULL, 0444);
MODULE_PARM_DESC(vid, "Vendor ID for the USB audio device.");
module_param_array(pid, int, NULL, 0444);
MODULE_PARM_DESC(pid, "Product ID for the USB audio device.");
module_param_array(device_setup, int, NULL, 0444);
MODULE_PARM_DESC(device_setup, "Specific device setup (if needed).");
module_param(ignore_ctl_error, bool, 0444);
MODULE_PARM_DESC(ignore_ctl_error,
		 "Ignore errors from USB controller for mixer interfaces.");
module_param(autoclock, bool, 0444);
MODULE_PARM_DESC(autoclock, "Enable auto-clock selection for UAC2 devices (default: yes).");
module_param_array(quirk_alias, charp, NULL, 0444);
MODULE_PARM_DESC(quirk_alias, "Quirk aliases, e.g. 0123abcd:5678beef.");

/*
 * we keep the snd_usb_audio_t instances by ourselves for merging
 * the all interfaces on the same card as one sound device.
 */

static DEFINE_MUTEX(register_mutex);
static struct snd_usb_audio *usb_chip[SNDRV_CARDS];
static struct usb_driver usb_audio_driver;

/*
 * disconnect streams
 * called from usb_audio_disconnect()
 */
static void snd_usb_stream_disconnect(struct snd_usb_stream *as)
{
	int idx;
	struct snd_usb_substream *subs;

	for (idx = 0; idx < 2; idx++) {
		subs = &as->substream[idx];
		if (!subs->num_formats)
			continue;
		subs->interface = -1;
		subs->data_endpoint = NULL;
		subs->sync_endpoint = NULL;
	}
}

static int snd_usb_create_stream(struct snd_usb_audio *chip, int ctrlif, int interface)
{
	struct usb_device *dev = chip->dev;
	struct usb_host_interface *alts;
	struct usb_interface_descriptor *altsd;
	struct usb_interface *iface = usb_ifnum_to_if(dev, interface);

	if (!iface) {
		dev_err(&dev->dev, "%u:%d : does not exist\n",
			ctrlif, interface);
		return -EINVAL;
	}

	alts = &iface->altsetting[0];
	altsd = get_iface_desc(alts);

	/*
	 * Android with both accessory and audio interfaces enabled gets the
	 * interface numbers wrong.
	 */
	if ((chip->usb_id == USB_ID(0x18d1, 0x2d04) ||
	     chip->usb_id == USB_ID(0x18d1, 0x2d05)) &&
	    interface == 0 &&
	    altsd->bInterfaceClass == USB_CLASS_VENDOR_SPEC &&
	    altsd->bInterfaceSubClass == USB_SUBCLASS_VENDOR_SPEC) {
		interface = 2;
		iface = usb_ifnum_to_if(dev, interface);
		if (!iface)
			return -EINVAL;
		alts = &iface->altsetting[0];
		altsd = get_iface_desc(alts);
	}

	if (usb_interface_claimed(iface)) {
		dev_dbg(&dev->dev, "%d:%d: skipping, already claimed\n",
			ctrlif, interface);
		return -EINVAL;
	}

	if ((altsd->bInterfaceClass == USB_CLASS_AUDIO ||
	     altsd->bInterfaceClass == USB_CLASS_VENDOR_SPEC) &&
	    altsd->bInterfaceSubClass == USB_SUBCLASS_MIDISTREAMING) {
		int err = __snd_usbmidi_create(chip->card, iface,
					     &chip->midi_list, NULL,
					     chip->usb_id);
		if (err < 0) {
			dev_err(&dev->dev,
				"%u:%d: cannot create sequencer device\n",
				ctrlif, interface);
			return -EINVAL;
		}
		return usb_driver_claim_interface(&usb_audio_driver, iface,
						  USB_AUDIO_IFACE_UNUSED);
	}

	if ((altsd->bInterfaceClass != USB_CLASS_AUDIO &&
	     altsd->bInterfaceClass != USB_CLASS_VENDOR_SPEC) ||
	    altsd->bInterfaceSubClass != USB_SUBCLASS_AUDIOSTREAMING) {
		dev_dbg(&dev->dev,
			"%u:%d: skipping non-supported interface %d\n",
			ctrlif, interface, altsd->bInterfaceClass);
		/* skip non-supported classes */
		return -EINVAL;
	}

	if (snd_usb_get_speed(dev) == USB_SPEED_LOW) {
		dev_err(&dev->dev, "low speed audio streaming not supported\n");
		return -EINVAL;
	}

	if (! snd_usb_parse_audio_interface(chip, interface)) {
		usb_set_interface(dev, interface, 0); /* reset the current interface */
		return usb_driver_claim_interface(&usb_audio_driver, iface,
						  USB_AUDIO_IFACE_UNUSED);
	}

	return 0;
}

/*
 * parse audio control descriptor and create pcm/midi streams
 */
static int snd_usb_create_streams(struct snd_usb_audio *chip, int ctrlif)
{
	struct usb_device *dev = chip->dev;
	struct usb_host_interface *host_iface;
	struct usb_interface_descriptor *altsd;
	void *control_header;
	int i, protocol;
	int rest_bytes;

	/* find audiocontrol interface */
	host_iface = &usb_ifnum_to_if(dev, ctrlif)->altsetting[0];
	control_header = snd_usb_find_csint_desc(host_iface->extra,
						 host_iface->extralen,
						 NULL, UAC_HEADER);
	altsd = get_iface_desc(host_iface);
	protocol = altsd->bInterfaceProtocol;

	if (!control_header) {
		dev_err(&dev->dev, "cannot find UAC_HEADER\n");
		return -EINVAL;
	}

	rest_bytes = (void *)(host_iface->extra + host_iface->extralen) -
		control_header;

	/* just to be sure -- this shouldn't hit at all */
	if (rest_bytes <= 0) {
		dev_err(&dev->dev, "invalid control header\n");
		return -EINVAL;
	}

	switch (protocol) {
	default:
		dev_warn(&dev->dev,
			 "unknown interface protocol %#02x, assuming v1\n",
			 protocol);
		/* fall through */

	case UAC_VERSION_1: {
		struct uac1_ac_header_descriptor *h1 = control_header;

		if (rest_bytes < sizeof(*h1)) {
			dev_err(&dev->dev, "too short v1 buffer descriptor\n");
			return -EINVAL;
		}

		if (!h1->bInCollection) {
			dev_info(&dev->dev, "skipping empty audio interface (v1)\n");
			return -EINVAL;
		}

		if (rest_bytes < h1->bLength) {
			dev_err(&dev->dev, "invalid buffer length (v1)\n");
			return -EINVAL;
		}

		if (h1->bLength < sizeof(*h1) + h1->bInCollection) {
			dev_err(&dev->dev, "invalid UAC_HEADER (v1)\n");
			return -EINVAL;
		}

		for (i = 0; i < h1->bInCollection; i++)
			snd_usb_create_stream(chip, ctrlif, h1->baInterfaceNr[i]);

		break;
	}

	case UAC_VERSION_2: {
		struct usb_interface_assoc_descriptor *assoc =
			usb_ifnum_to_if(dev, ctrlif)->intf_assoc;

		if (!assoc) {
			/*
			 * Firmware writers cannot count to three.  So to find
			 * the IAD on the NuForce UDH-100, also check the next
			 * interface.
			 */
			struct usb_interface *iface =
				usb_ifnum_to_if(dev, ctrlif + 1);
			if (iface &&
			    iface->intf_assoc &&
			    iface->intf_assoc->bFunctionClass == USB_CLASS_AUDIO &&
			    iface->intf_assoc->bFunctionProtocol == UAC_VERSION_2)
				assoc = iface->intf_assoc;
		}

		if (!assoc) {
			dev_err(&dev->dev, "Audio class v2 interfaces need an interface association\n");
			return -EINVAL;
		}

		for (i = 0; i < assoc->bInterfaceCount; i++) {
			int intf = assoc->bFirstInterface + i;

			if (intf != ctrlif)
				snd_usb_create_stream(chip, ctrlif, intf);
		}

		break;
	}
	}

	return 0;
}

/*
 * free the chip instance
 *
 * here we have to do not much, since pcm and controls are already freed
 *
 */

static int snd_usb_audio_free(struct snd_usb_audio *chip)
{
	struct snd_usb_endpoint *ep, *n;

	list_for_each_entry_safe(ep, n, &chip->ep_list, list)
		snd_usb_endpoint_free(ep);

	mutex_destroy(&chip->mutex);
	if (!atomic_read(&chip->shutdown))
		dev_set_drvdata(&chip->dev->dev, NULL);
	kfree(chip);
	return 0;
}

static int snd_usb_audio_dev_free(struct snd_device *device)
{
	struct snd_usb_audio *chip = device->device_data;
	return snd_usb_audio_free(chip);
}

/*
 * create a chip instance and set its names.
 */
static int snd_usb_audio_create(struct usb_interface *intf,
				struct usb_device *dev, int idx,
				const struct snd_usb_audio_quirk *quirk,
				unsigned int usb_id,
				struct snd_usb_audio **rchip)
{
	struct snd_card *card;
	struct snd_usb_audio *chip;
	int err, len;
	char component[14];
	static struct snd_device_ops ops = {
		.dev_free =	snd_usb_audio_dev_free,
	};

	*rchip = NULL;

	switch (snd_usb_get_speed(dev)) {
	case USB_SPEED_LOW:
	case USB_SPEED_FULL:
	case USB_SPEED_HIGH:
	case USB_SPEED_WIRELESS:
	case USB_SPEED_SUPER:
	case USB_SPEED_SUPER_PLUS:
		break;
	default:
		dev_err(&dev->dev, "unknown device speed %d\n", snd_usb_get_speed(dev));
		return -ENXIO;
	}

	err = snd_card_new(&intf->dev, index[idx], id[idx], THIS_MODULE,
			   0, &card);
	if (err < 0) {
		dev_err(&dev->dev, "cannot create card instance %d\n", idx);
		return err;
	}

	chip = kzalloc(sizeof(*chip), GFP_KERNEL);
	if (! chip) {
		snd_card_free(card);
		return -ENOMEM;
	}

	mutex_init(&chip->mutex);
	init_waitqueue_head(&chip->shutdown_wait);
	chip->index = idx;
	chip->dev = dev;
	chip->card = card;
	chip->setup = device_setup[idx];
	chip->autoclock = autoclock;
	atomic_set(&chip->active, 1); /* avoid autopm during probing */
	atomic_set(&chip->usage_count, 0);
	atomic_set(&chip->shutdown, 0);

	chip->usb_id = usb_id;
	INIT_LIST_HEAD(&chip->pcm_list);
	INIT_LIST_HEAD(&chip->ep_list);
	INIT_LIST_HEAD(&chip->midi_list);
	INIT_LIST_HEAD(&chip->mixer_list);

	if ((err = snd_device_new(card, SNDRV_DEV_LOWLEVEL, chip, &ops)) < 0) {
		snd_usb_audio_free(chip);
		snd_card_free(card);
		return err;
	}

	strcpy(card->driver, "USB-Audio");
	sprintf(component, "USB%04x:%04x",
		USB_ID_VENDOR(chip->usb_id), USB_ID_PRODUCT(chip->usb_id));
	snd_component_add(card, component);

	/* retrieve the device string as shortname */
	if (quirk && quirk->product_name && *quirk->product_name) {
		strlcpy(card->shortname, quirk->product_name, sizeof(card->shortname));
	} else {
		if (!dev->descriptor.iProduct ||
		    usb_string(dev, dev->descriptor.iProduct,
		    card->shortname, sizeof(card->shortname)) <= 0) {
			/* no name available from anywhere, so use ID */
			sprintf(card->shortname, "USB Device %#04x:%#04x",
				USB_ID_VENDOR(chip->usb_id),
				USB_ID_PRODUCT(chip->usb_id));
		}
	}
	strim(card->shortname);

	/* retrieve the vendor and device strings as longname */
	if (quirk && quirk->vendor_name && *quirk->vendor_name) {
		len = strlcpy(card->longname, quirk->vendor_name, sizeof(card->longname));
	} else {
		if (dev->descriptor.iManufacturer)
			len = usb_string(dev, dev->descriptor.iManufacturer,
					 card->longname, sizeof(card->longname));
		else
			len = 0;
		/* we don't really care if there isn't any vendor string */
	}
	if (len > 0) {
		strim(card->longname);
		if (*card->longname)
			strlcat(card->longname, " ", sizeof(card->longname));
	}

	strlcat(card->longname, card->shortname, sizeof(card->longname));

	len = strlcat(card->longname, " at ", sizeof(card->longname));

	if (len < sizeof(card->longname))
		usb_make_path(dev, card->longname + len, sizeof(card->longname) - len);

	switch (snd_usb_get_speed(dev)) {
	case USB_SPEED_LOW:
		strlcat(card->longname, ", low speed", sizeof(card->longname));
		break;
	case USB_SPEED_FULL:
		strlcat(card->longname, ", full speed", sizeof(card->longname));
		break;
	case USB_SPEED_HIGH:
		strlcat(card->longname, ", high speed", sizeof(card->longname));
		break;
	case USB_SPEED_SUPER:
		strlcat(card->longname, ", super speed", sizeof(card->longname));
		break;
	case USB_SPEED_SUPER_PLUS:
		strlcat(card->longname, ", super speed plus", sizeof(card->longname));
		break;
	default:
		break;
	}

	snd_usb_audio_create_proc(chip);

	*rchip = chip;
	return 0;
}

/* look for a matching quirk alias id */
static bool get_alias_id(struct usb_device *dev, unsigned int *id)
{
	int i;
	unsigned int src, dst;

	for (i = 0; i < ARRAY_SIZE(quirk_alias); i++) {
		if (!quirk_alias[i] ||
		    sscanf(quirk_alias[i], "%x:%x", &src, &dst) != 2 ||
		    src != *id)
			continue;
		dev_info(&dev->dev,
			 "device (%04x:%04x): applying quirk alias %04x:%04x\n",
			 USB_ID_VENDOR(*id), USB_ID_PRODUCT(*id),
			 USB_ID_VENDOR(dst), USB_ID_PRODUCT(dst));
		*id = dst;
		return true;
	}

	return false;
}

static const struct usb_device_id usb_audio_ids[]; /* defined below */

/* look for the corresponding quirk */
static const struct snd_usb_audio_quirk *
get_alias_quirk(struct usb_device *dev, unsigned int id)
{
	const struct usb_device_id *p;

	for (p = usb_audio_ids; p->match_flags; p++) {
		/* FIXME: this checks only vendor:product pair in the list */
		if ((p->match_flags & USB_DEVICE_ID_MATCH_DEVICE) ==
		    USB_DEVICE_ID_MATCH_DEVICE &&
		    p->idVendor == USB_ID_VENDOR(id) &&
		    p->idProduct == USB_ID_PRODUCT(id))
			return (const struct snd_usb_audio_quirk *)p->driver_info;
	}

	return NULL;
}

/*
 * probe the active usb device
 *
 * note that this can be called multiple times per a device, when it
 * includes multiple audio control interfaces.
 *
 * thus we check the usb device pointer and creates the card instance
 * only at the first time.  the successive calls of this function will
 * append the pcm interface to the corresponding card.
 */
static int usb_audio_probe(struct usb_interface *intf,
			   const struct usb_device_id *usb_id)
{
	struct usb_device *dev = interface_to_usbdev(intf);
	const struct snd_usb_audio_quirk *quirk =
		(const struct snd_usb_audio_quirk *)usb_id->driver_info;
	struct snd_usb_audio *chip;
	int i, err;
	struct usb_host_interface *alts;
	int ifnum;
	u32 id;
#ifdef CONFIG_SND_EXYNOS_USB_AUDIO
	struct usb_interface_descriptor *altsd;
	int timeout = 0;
#endif

	alts = &intf->altsetting[0];
	ifnum = get_iface_desc(alts)->bInterfaceNumber;
	id = USB_ID(le16_to_cpu(dev->descriptor.idVendor),
		    le16_to_cpu(dev->descriptor.idProduct));
	if (get_alias_id(dev, &id))
		quirk = get_alias_quirk(dev, id);
	if (quirk && quirk->ifnum >= 0 && ifnum != quirk->ifnum)
		return -ENXIO;

	err = snd_usb_apply_boot_quirk(dev, intf, quirk, id);
	if (err < 0)
		return err;

#ifdef CONFIG_SND_EXYNOS_USB_AUDIO
	altsd = get_iface_desc(alts);
	if ((altsd->bInterfaceClass == USB_CLASS_AUDIO ||
		altsd->bInterfaceClass == USB_CLASS_VENDOR_SPEC) &&
		altsd->bInterfaceSubClass == USB_SUBCLASS_MIDISTREAMING) {
			pr_info("USB_AUDIO_IPC : %s - MIDI device detected!\n", __func__);
	} else {
		pr_info("USB_AUDIO_IPC : %s - No MIDI device detected!\n", __func__);

		if (usb_audio->usb_audio_state == USB_AUDIO_REMOVING) {
			timeout =
				wait_for_completion_timeout(&usb_audio
					->discon_done,
					msecs_to_jiffies(DISCONNECT_TIMEOUT));
			pr_info("%s: wait disconnect %dmsec",
						__func__, timeout);

			if (!timeout && (usb_audio->usb_audio_state ==
					USB_AUDIO_REMOVING)) {
				usb_audio->usb_audio_state =
						USB_AUDIO_TIMEOUT_PROBE;
				pr_err("%s: timeout for disconnect\n",
					__func__);
			}
		}

		if ((usb_audio->usb_audio_state ==
				USB_AUDIO_DISCONNECT) ||
				(usb_audio->usb_audio_state ==
				USB_AUDIO_TIMEOUT_PROBE)) {
			pr_info("USB_AUDIO_IPC : %s - USB Audio set!\n", __func__);
			exynos_usb_audio_set_device(dev);
			exynos_usb_audio_conn(1);
			exynos_usb_audio_hcd(dev);
			exynos_usb_audio_desc(dev);
			exynos_usb_audio_map_buf(dev);
			usb_audio->is_audio = 1;
		}
	}
#endif

	/*
	 * found a config.  now register to ALSA
	 */

	/* check whether it's already registered */
	chip = NULL;
	mutex_lock(&register_mutex);
	for (i = 0; i < SNDRV_CARDS; i++) {
		if (usb_chip[i] && usb_chip[i]->dev == dev) {
			if (atomic_read(&usb_chip[i]->shutdown)) {
				dev_err(&dev->dev, "USB device is in the shutdown state, cannot create a card instance\n");
				err = -EIO;
				goto __error;
			}
			chip = usb_chip[i];
			atomic_inc(&chip->active); /* avoid autopm */
			break;
		}
	}
	if (! chip) {
		/* it's a fresh one.
		 * now look for an empty slot and create a new card instance
		 */
		for (i = 0; i < SNDRV_CARDS; i++)
			if (enable[i] && ! usb_chip[i] &&
			    (vid[i] == -1 || vid[i] == USB_ID_VENDOR(id)) &&
			    (pid[i] == -1 || pid[i] == USB_ID_PRODUCT(id))) {
				err = snd_usb_audio_create(intf, dev, i, quirk,
							   id, &chip);
				if (err < 0)
					goto __error;
				chip->pm_intf = intf;
				break;
			}
		if (!chip) {
			dev_err(&dev->dev, "no available usb audio device\n");
			err = -ENODEV;
			goto __error;
		}
	}
	dev_set_drvdata(&dev->dev, chip);

	/*
	 * For devices with more than one control interface, we assume the
	 * first contains the audio controls. We might need a more specific
	 * check here in the future.
	 */
	if (!chip->ctrl_intf)
		chip->ctrl_intf = alts;

	chip->txfr_quirk = 0;
	err = 1; /* continue */
	if (quirk && quirk->ifnum != QUIRK_NO_INTERFACE) {
		/* need some special handlings */
		err = snd_usb_create_quirk(chip, intf, &usb_audio_driver, quirk);
		if (err < 0)
			goto __error;
	}

	if (err > 0) {
		/* create normal USB audio interfaces */
		err = snd_usb_create_streams(chip, ifnum);
		if (err < 0)
			goto __error;
		err = snd_usb_create_mixer(chip, ifnum, ignore_ctl_error);
		if (err < 0)
			goto __error;
	}

	/* we are allowed to call snd_card_register() many times */
	err = snd_card_register(chip->card);
	if (err < 0)
		goto __error;

	usb_chip[chip->index] = chip;
	chip->num_interfaces++;
	usb_set_intfdata(intf, chip);
	atomic_dec(&chip->active);
	mutex_unlock(&register_mutex);

	if (dev->do_remote_wakeup)
		usb_enable_autosuspend(dev);

	pr_info("%s done\n", __func__);

	return 0;

 __error:
	if (chip) {
		/* chip->active is inside the chip->card object,
		 * decrement before memory is possibly returned.
		 */
		atomic_dec(&chip->active);
		if (!chip->num_interfaces)
			snd_card_free(chip->card);
	}
	mutex_unlock(&register_mutex);
	return err;
}

/*
 * we need to take care of counter, since disconnection can be called also
 * many times as well as usb_audio_probe().
 */
static void usb_audio_disconnect(struct usb_interface *intf)
{
	struct snd_usb_audio *chip = usb_get_intfdata(intf);
	struct snd_card *card;
	struct list_head *p;

	if (chip == USB_AUDIO_IFACE_UNUSED)
		return;

	card = chip->card;

#ifdef CONFIG_SND_EXYNOS_USB_AUDIO
	exynos_usb_audio_conn(0);
#endif

	mutex_lock(&register_mutex);
	if (atomic_inc_return(&chip->shutdown) == 1) {
		struct snd_usb_stream *as;
		struct snd_usb_endpoint *ep;
		struct usb_mixer_interface *mixer;

		/* wait until all pending tasks done;
		 * they are protected by snd_usb_lock_shutdown()
		 */
		wait_event(chip->shutdown_wait,
			   !atomic_read(&chip->usage_count));
		snd_card_disconnect(card);
		/* release the pcm resources */
		list_for_each_entry(as, &chip->pcm_list, list) {
			snd_usb_stream_disconnect(as);
		}
		/* release the endpoint resources */
		list_for_each_entry(ep, &chip->ep_list, list) {
			snd_usb_endpoint_release(ep);
		}
		/* release the midi resources */
		list_for_each(p, &chip->midi_list) {
			snd_usbmidi_disconnect(p);
		}
		/* release mixer resources */
		list_for_each_entry(mixer, &chip->mixer_list, list) {
			snd_usb_mixer_disconnect(mixer);
		}
	}

	chip->num_interfaces--;
	if (chip->num_interfaces <= 0) {
		usb_chip[chip->index] = NULL;
		mutex_unlock(&register_mutex);
		snd_card_free_when_closed(card);
	} else {
		mutex_unlock(&register_mutex);
	}
}

/* lock the shutdown (disconnect) task and autoresume */
int snd_usb_lock_shutdown(struct snd_usb_audio *chip)
{
	int err;

	atomic_inc(&chip->usage_count);
	if (atomic_read(&chip->shutdown)) {
		err = -EIO;
		goto error;
	}
	err = snd_usb_autoresume(chip);
	if (err < 0)
		goto error;
	return 0;

 error:
	if (atomic_dec_and_test(&chip->usage_count))
		wake_up(&chip->shutdown_wait);
	return err;
}

/* autosuspend and unlock the shutdown */
void snd_usb_unlock_shutdown(struct snd_usb_audio *chip)
{
	snd_usb_autosuspend(chip);
	if (atomic_dec_and_test(&chip->usage_count))
		wake_up(&chip->shutdown_wait);
}

#ifdef CONFIG_PM

int snd_usb_autoresume(struct snd_usb_audio *chip)
{
	if (atomic_read(&chip->shutdown))
		return -EIO;
	if (atomic_inc_return(&chip->active) == 1)
		return usb_autopm_get_interface(chip->pm_intf);
	return 0;
}

void snd_usb_autosuspend(struct snd_usb_audio *chip)
{
	if (atomic_read(&chip->shutdown))
		return;
	if (atomic_dec_and_test(&chip->active))
		usb_autopm_put_interface(chip->pm_intf);
}

static int usb_audio_suspend(struct usb_interface *intf, pm_message_t message)
{
	struct snd_usb_audio *chip = usb_get_intfdata(intf);
	struct snd_usb_stream *as;
	struct usb_mixer_interface *mixer;
	struct list_head *p;

	if (chip == USB_AUDIO_IFACE_UNUSED)
		return 0;

	if (!chip->num_suspended_intf++) {
		list_for_each_entry(as, &chip->pcm_list, list) {
			snd_pcm_suspend_all(as->pcm);
			as->substream[0].need_setup_ep =
				as->substream[1].need_setup_ep = true;
		}
		list_for_each(p, &chip->midi_list)
			snd_usbmidi_suspend(p);
		list_for_each_entry(mixer, &chip->mixer_list, list)
			snd_usb_mixer_suspend(mixer);
	}

	if (!PMSG_IS_AUTO(message) && !chip->system_suspend) {
		snd_power_change_state(chip->card, SNDRV_CTL_POWER_D3hot);
		chip->system_suspend = chip->num_suspended_intf;
	}

	return 0;
}

static int __usb_audio_resume(struct usb_interface *intf, bool reset_resume)
{
	struct snd_usb_audio *chip = usb_get_intfdata(intf);
	struct usb_mixer_interface *mixer;
	struct list_head *p;
	int err = 0;

	if (chip == USB_AUDIO_IFACE_UNUSED)
		return 0;

	atomic_inc(&chip->active); /* avoid autopm */
	if (chip->num_suspended_intf > 1)
		goto out;

	/*
	 * ALSA leaves material resumption to user space
	 * we just notify and restart the mixers
	 */
	list_for_each_entry(mixer, &chip->mixer_list, list) {
		err = snd_usb_mixer_resume(mixer, reset_resume);
		if (err < 0)
			goto err_out;
	}

	list_for_each(p, &chip->midi_list) {
		snd_usbmidi_resume(p);
	}

 out:
	if (chip->num_suspended_intf == chip->system_suspend) {
		snd_power_change_state(chip->card, SNDRV_CTL_POWER_D0);
		chip->system_suspend = 0;
	}
	chip->num_suspended_intf--;

err_out:
	atomic_dec(&chip->active); /* allow autopm after this point */
	return err;
}

static int usb_audio_resume(struct usb_interface *intf)
{
	return __usb_audio_resume(intf, false);
}

static int usb_audio_reset_resume(struct usb_interface *intf)
{
	return __usb_audio_resume(intf, true);
}
#else
#define usb_audio_suspend	NULL
#define usb_audio_resume	NULL
#define usb_audio_reset_resume	NULL
#endif		/* CONFIG_PM */

static const struct usb_device_id usb_audio_ids [] = {
#include "quirks-table.h"
    { .match_flags = (USB_DEVICE_ID_MATCH_INT_CLASS | USB_DEVICE_ID_MATCH_INT_SUBCLASS),
      .bInterfaceClass = USB_CLASS_AUDIO,
      .bInterfaceSubClass = USB_SUBCLASS_AUDIOCONTROL },
    { }						/* Terminating entry */
};
MODULE_DEVICE_TABLE(usb, usb_audio_ids);

/*
 * entry point for linux usb interface
 */

static struct usb_driver usb_audio_driver = {
	.name =		"snd-usb-audio",
	.probe =	usb_audio_probe,
	.disconnect =	usb_audio_disconnect,
	.suspend =	usb_audio_suspend,
	.resume =	usb_audio_resume,
	.reset_resume =	usb_audio_reset_resume,
	.id_table =	usb_audio_ids,
	.supports_autosuspend = 1,
};

module_usb_driver(usb_audio_driver);
