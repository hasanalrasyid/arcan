-- set_led_rgb
-- @short: Set the specific R,G,B values for an individual LED on a specific controller.
-- @inargs: controlid, ledid, r, g, b, *buffer*
-- @outarg: bool
-- @group: iodev
-- @cfunction: led_rgb
-- @longdescr: Set the color value of an individual LED on a known LED controller and
-- returns -1 if the *controlid* does not exist, if *ledid* is not a valid index,
-- if the *controlid* backed device lacks the r,g,b capability. A negative *ledid*
-- will result in the value being set for all LEDs associated with the device.
-- A return of 0 means that the update failed as it would block the device. This
-- can happen if many LED update queries are being pushed to a slow device.
-- If the optional boolean *buffer* is set, the request may be queued and the queue
-- will not dispatch until a non-buffered update is called.
